#!/usr/bin/env python3
"""Build the narrowed LWIR experiment report using only the standard library."""

from __future__ import annotations

import argparse
import csv
import html
import json
import math
import pathlib
import re
import statistics


STAGES = {
    "capture_processing": ("rpi/capture_metrics.csv", "processing_ms"),
    "preprocess_queue_wait": ("rpi/preprocess_metrics.csv", "queue_wait_ms"),
    "preprocess_processing": ("rpi/preprocess_metrics.csv", "processing_ms"),
    "streaming_queue_wait": ("rpi/streaming_metrics.csv", "queue_wait_ms"),
    "streaming_submit": ("rpi/streaming_metrics.csv", "processing_ms"),
    "encoding": ("rpi/encoding_metrics.csv", "encoding_ms"),
}
ORDER = ["B0", "Q1", "E1", "E2", "B1"]
LABELS = {
    "B0": "Baseline",
    "Q1": "Queue depth=2",
    "E1": "tune=zerolatency",
    "E2": "speed-preset=ultrafast",
    "B1": "Optimized",
}


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def percentile(values: list[float], fraction: float) -> float:
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return values[lower]
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def active_fps(path: pathlib.Path, stage: str | None = None) -> float | None:
    rows = read_csv(path)
    if stage is not None:
        rows = [row for row in rows if row.get("stage") == stage]
    rows = [row for row in rows if int(row["frame_count"]) > 0]
    if len(rows) > 2:
        rows = rows[1:-1]
    duration = sum(
        float(row["interval_end_s"]) - float(row["interval_start_s"])
        for row in rows
    )
    return sum(int(row["frame_count"]) for row in rows) / duration if duration else None


def resource_values(path: pathlib.Path) -> tuple[float | None, float | None]:
    if not path.is_file():
        return None, None
    text = path.read_text(encoding="utf-8")
    cpu = re.search(r"Percent of CPU this job got:\s*([0-9.]+)%", text)
    rss = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", text)
    return (
        float(cpu.group(1)) if cpu else None,
        float(rss.group(1)) / 1024 if rss else None,
    )


def run_summary(condition: str, run_dir: pathlib.Path) -> dict[str, object]:
    manifest = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
    warmup = int(manifest.get("warmup_frames", 60))
    result: dict[str, object] = {
        "condition": condition,
        "run": run_dir.name,
        "source": str(run_dir),
    }
    for name, (relative, column) in STAGES.items():
        values = []
        for row in read_csv(run_dir / relative):
            if row.get("frame_id") and int(row["frame_id"]) <= warmup:
                continue
            if row.get(column):
                values.append(float(row[column]))
        result[f"{name}_mean_ms"] = statistics.fmean(values) if values else None
        result[f"{name}_p95_ms"] = percentile(values, 0.95) if values else None

    frame = read_csv(run_dir / "rpi/frame_metrics.csv")
    receiver = read_csv(run_dir / "pc/receiver_frame_metrics.csv")
    network = read_csv(run_dir / "rpi/network_metrics.csv")
    sent = int(frame[0]["streaming_submit_success_count"]) if frame else None
    decoded = int(receiver[0]["decoded_frame_count"]) if receiver else None
    result.update(
        sender_fps=active_fps(run_dir / "rpi/fps_metrics.csv", "streaming_submit"),
        receiver_fps=active_fps(run_dir / "pc/receiver_fps_metrics.csv"),
        sent_frames=sent,
        decoded_frames=decoded,
        frame_gap=(sent - decoded) if sent is not None and decoded is not None else None,
        frame_gap_pct=(100 * (sent - decoded) / sent) if sent and decoded is not None else None,
        queue_policy_drops=(int(frame[0]["queue_policy_drop_count"]) if frame else None),
        bitrate_kbps=(float(network[0]["average_bitrate_kbps"]) if network else None),
    )
    sender_cpu, sender_rss = resource_values(run_dir / "rpi/resource_usage.txt")
    receiver_cpu, receiver_rss = resource_values(run_dir / "pc/resource_usage.txt")
    result.update(
        sender_cpu_pct=sender_cpu,
        sender_rss_mb=sender_rss,
        receiver_cpu_pct=receiver_cpu,
        receiver_rss_mb=receiver_rss,
    )

    matched = int(receiver[0]["metadata_matched_frame_count"]) if receiver else 0
    result["e2e_match_coverage_pct"] = 100 * matched / decoded if decoded else 0.0
    result["e2e_valid"] = bool(decoded and matched / decoded >= 0.95)
    return result


def mean_present(rows: list[dict[str, object]], key: str) -> float | None:
    values = [float(row[key]) for row in rows if row.get(key) is not None]
    return statistics.fmean(values) if values else None


def aggregate(runs: list[dict[str, object]]) -> list[dict[str, object]]:
    conditions = []
    for condition in ORDER:
        rows = [row for row in runs if row["condition"] == condition]
        if not rows:
            continue
        record: dict[str, object] = {"condition": condition, "runs": len(rows)}
        numeric_keys = [key for key in rows[0] if key not in {"condition", "run", "source", "e2e_valid"}]
        for key in numeric_keys:
            record[key] = mean_present(rows, key)
        record["e2e_valid"] = all(bool(row["e2e_valid"]) for row in rows)
        conditions.append(record)
    return conditions


def write_csv(path: pathlib.Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def f(value: object, digits: int = 2) -> str:
    if value is None:
        return "N/A"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def svg_latency(rows: list[dict[str, object]], path: pathlib.Path) -> None:
    series = [(row["condition"], float(row["encoding_mean_ms"])) for row in rows]
    maximum = max(value for _, value in series)
    height = 80 + len(series) * 54
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="960" height="{height}" viewBox="0 0 960 {height}">',
        '<rect width="100%" height="100%" fill="#fff"/>',
        '<style>text{font-family:Arial,sans-serif;fill:#172033}.label{font-size:15px}.value{font-size:14px;font-weight:700}</style>',
        '<text x="24" y="32" font-size="20" font-weight="700">Encoding latency (run-mean average)</text>',
    ]
    colors = {"B0": "#64748b", "Q1": "#eab308", "E1": "#0ea5e9", "E2": "#8b5cf6", "B1": "#16a34a"}
    for index, (condition, value) in enumerate(series):
        y = 58 + index * 54
        width = 680 * value / maximum
        parts.extend([
            f'<text class="label" x="24" y="{y + 23}">{condition} · {html.escape(LABELS[condition])}</text>',
            f'<rect x="205" y="{y}" width="{width:.1f}" height="30" rx="4" fill="{colors[condition]}"/>',
            f'<text class="value" x="{215 + width:.1f}" y="{y + 21}">{value:.2f} ms</text>',
        ])
    parts.append("</svg>")
    path.write_text("".join(parts), encoding="utf-8")


def svg_system(rows: list[dict[str, object]], path: pathlib.Path) -> None:
    width, height = 1040, 390
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#fff"/>',
        '<style>text{font-family:Arial,sans-serif;fill:#172033}.small{font-size:13px}.title{font-size:18px;font-weight:700}</style>',
    ]
    panels = [
        ("Receiver FPS", "receiver_fps", 36.0, "fps"),
        ("Sender CPU", "sender_cpu_pct", 350.0, "%"),
        ("RTP bitrate", "bitrate_kbps", 110.0, "kbps"),
    ]
    colors = ["#0ea5e9", "#f97316", "#8b5cf6", "#16a34a", "#64748b"]
    for panel, (title, key, scale, unit) in enumerate(panels):
        x0 = 24 + panel * 338
        parts.append(f'<text class="title" x="{x0}" y="30">{title}</text>')
        for index, row in enumerate(rows):
            value = float(row[key])
            y = 50 + index * 60
            bar = 250 * value / scale
            parts.extend([
                f'<text class="small" x="{x0}" y="{y + 18}">{row["condition"]}</text>',
                f'<rect x="{x0 + 38}" y="{y}" width="{bar:.1f}" height="24" rx="3" fill="{colors[index]}"/>',
                f'<text class="small" x="{x0 + 45 + bar:.1f}" y="{y + 17}">{value:.2f} {unit}</text>',
            ])
    parts.append("</svg>")
    path.write_text("".join(parts), encoding="utf-8")


def comparison_rows(conditions: list[dict[str, object]]) -> list[dict[str, object]]:
    by_id = {row["condition"]: row for row in conditions}
    if "B0" not in by_id or "B1" not in by_id:
        return []
    baseline, optimized = by_id["B0"], by_id["B1"]
    metrics = [
        ("capture_processing_mean_ms", "Capture processing mean", "ms", True),
        ("capture_processing_p95_ms", "Capture processing P95", "ms", True),
        ("preprocess_queue_wait_mean_ms", "Preprocess queue mean", "ms", True),
        ("preprocess_queue_wait_p95_ms", "Preprocess queue P95", "ms", True),
        ("encoding_mean_ms", "Encoding mean", "ms", True),
        ("encoding_p95_ms", "Encoding P95", "ms", True),
        ("preprocess_processing_mean_ms", "Preprocess mean", "ms", True),
        ("preprocess_processing_p95_ms", "Preprocess P95", "ms", True),
        ("streaming_queue_wait_mean_ms", "Streaming queue mean", "ms", True),
        ("streaming_queue_wait_p95_ms", "Streaming queue P95", "ms", True),
        ("streaming_submit_mean_ms", "Streaming submit mean", "ms", True),
        ("streaming_submit_p95_ms", "Streaming submit P95", "ms", True),
        ("receiver_fps", "Receiver FPS", "fps", False),
        ("frame_gap_pct", "Frame gap", "%", True),
        ("sender_cpu_pct", "Sender CPU", "%", True),
        ("sender_rss_mb", "Sender max RSS", "MiB", True),
        ("bitrate_kbps", "RTP bitrate", "kbps", True),
    ]
    output = []
    for key, label, unit, lower_better in metrics:
        before, after = float(baseline[key]), float(optimized[key])
        change = 100 * (after - before) / before if before else None
        improvement = -change if lower_better and change is not None else change
        output.append({
            "metric": label,
            "unit": unit,
            "B0": before,
            "B1": after,
            "change_pct": change,
            "improvement_pct": improvement,
        })
    return output


def write_html(
    path: pathlib.Path,
    conditions: list[dict[str, object]],
    comparisons: list[dict[str, object]],
    root: pathlib.Path,
) -> None:
    by_id = {row["condition"]: row for row in conditions}
    condition_rows = []
    for row in conditions:
        condition_rows.append(
            "<tr>" + "".join(f"<td>{html.escape(value)}</td>" for value in [
                str(row["condition"]), LABELS[str(row["condition"])], f(row["runs"], 0),
                f(row["encoding_mean_ms"]), f(row["encoding_p95_ms"]),
                f(row["preprocess_queue_wait_mean_ms"]), f(row["streaming_queue_wait_mean_ms"]),
                f(row["sender_fps"]), f(row["receiver_fps"]), f(row["frame_gap_pct"]),
                f(row["sender_cpu_pct"]), f(row["sender_rss_mb"]), f(row["bitrate_kbps"]),
            ]) + "</tr>"
        )
    comparison_html = "<p class='pending'>B1 측정 전: 최종 B0/B1 비교는 아직 생성되지 않았습니다.</p>"
    if comparisons:
        comparison_html = "<table><thead><tr><th>Metric</th><th>Unit</th><th>B0</th><th>B1</th><th>Change</th></tr></thead><tbody>" + "".join(
            f"<tr><td>{row['metric']}</td><td>{row['unit']}</td><td>{f(row['B0'])}</td><td>{f(row['B1'])}</td><td>{f(row['change_pct'])}%</td></tr>"
            for row in comparisons
        ) + "</tbody></table>"
    stage_html = ""
    if "B0" in by_id and "B1" in by_id:
        stage_names = [
            ("Capture processing", "capture_processing"),
            ("Preprocess queue wait", "preprocess_queue_wait"),
            ("Preprocess processing", "preprocess_processing"),
            ("Streaming queue wait", "streaming_queue_wait"),
            ("Streaming submit", "streaming_submit"),
            ("Encoding", "encoding"),
        ]
        stage_html = "<h2>단계별 B0/B1 지연</h2><table><thead><tr><th>Stage</th><th>B0 avg ms</th><th>B0 P95 ms</th><th>B1 avg ms</th><th>B1 P95 ms</th></tr></thead><tbody>" + "".join(
            f"<tr><td>{name}</td><td>{f(by_id['B0'][key + '_mean_ms'])}</td><td>{f(by_id['B0'][key + '_p95_ms'])}</td><td>{f(by_id['B1'][key + '_mean_ms'])}</td><td>{f(by_id['B1'][key + '_p95_ms'])}</td></tr>"
            for name, key in stage_names
        ) + "</tbody></table>"
    b0_video = next(root.glob("B0/run-*/pc/B0.mp4"), None)
    b1_video = next(root.glob("B1/run-*/pc/B1.mp4"), None)
    video_text = f"B0: {b0_video.name if b0_video else '없음'} / B1: {b1_video.name if b1_video else '측정 전'}"
    document = f"""<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LWIR 최종 성능 보고서</title><style>
body{{font-family:Arial,'Noto Sans KR',sans-serif;margin:32px;color:#172033;max-width:1200px}}
h1,h2{{letter-spacing:-.02em}} .note,.pending{{padding:14px 16px;border-radius:8px;background:#fff7ed;border-left:4px solid #f97316}}
table{{border-collapse:collapse;width:100%;font-size:13px;margin:14px 0 28px}}th,td{{padding:8px 10px;border-bottom:1px solid #dbe2ea;text-align:right}}
th{{background:#f1f5f9}}th:first-child,td:first-child,th:nth-child(2),td:nth-child(2){{text-align:left}}img{{width:100%;max-width:1040px;border:1px solid #dbe2ea}}
.ok{{padding:14px 16px;background:#ecfdf5;border-left:4px solid #16a34a;border-radius:8px}}
</style></head><body>
<h1>LWIR 영상 송신 성능 평가</h1>
<p>Raspberry Pi 4 · 640×480 16-bit PNG 971장 · 30 FPS · x264enc · RTP/UDP</p>
<p class="note"><strong>E2E 지연 제외:</strong> RTP 메타데이터와 디코더 PTS의 매칭률이 95% 기준을 충족하지 못했습니다. 잘못된 프레임 ID 기반 손실률과 E2E 수치는 보고하지 않고, 송신 성공 수와 디코드 수의 직접 차이를 사용했습니다.</p>
<h2>조건별 결과</h2><table><thead><tr><th>ID</th><th>설정</th><th>n</th><th>Encode avg ms</th><th>Encode P95 ms</th><th>Pre-Q avg ms</th><th>Stream-Q avg ms</th><th>TX FPS</th><th>RX FPS</th><th>Frame gap %</th><th>Pi CPU %</th><th>Pi RSS MiB</th><th>RTP kbps</th></tr></thead><tbody>{''.join(condition_rows)}</tbody></table>
<h2>인코딩 지연</h2><img src="latency_comparison.svg" alt="조건별 인코딩 지연"><h2>처리량·CPU·비트레이트</h2><img src="system_comparison.svg" alt="조건별 처리량 CPU 비트레이트">
{stage_html}<h2>Baseline vs Optimized</h2>{comparison_html}
<h2>해석</h2><ul><li>Q1은 큐가 실제로 누적될 때만 효과가 있습니다. 현재 측정에서는 정책 드롭이 0이고 B0 대비 인코딩 지연이 거의 같아 B1에 포함하지 않습니다.</li><li>E1은 x264 look-ahead와 내부 버퍼링을 줄여 프레임 보류 시간을 크게 줄였습니다. 대신 CPU와 비트레이트가 증가할 수 있습니다.</li><li>E2는 압축 탐색을 줄이는 설정입니다. CPU·지연은 줄 수 있지만 같은 화질에서 비트레이트가 늘 수 있으므로 실제 측정값으로만 채택 여부를 결정합니다.</li></ul>
<h2>영상</h2><p>{html.escape(video_text)}</p>
</body></html>"""
    path.write_text(document, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=pathlib.Path)
    parser.add_argument("--baseline-run", action="append", default=[], type=pathlib.Path)
    args = parser.parse_args()
    root = args.campaign.resolve()
    run_dirs: list[tuple[str, pathlib.Path]] = []
    for condition in ORDER:
        run_dirs.extend((condition, path.parent) for path in sorted((root / condition).glob("run-*/manifest.json")))
    existing_b0 = [item for item in run_dirs if item[0] == "B0"]
    for baseline in args.baseline_run:
        resolved = baseline.resolve()
        if resolved not in [path for _, path in existing_b0]:
            run_dirs.append(("B0", resolved))
    runs = [run_summary(condition, path) for condition, path in run_dirs]
    conditions = aggregate(runs)
    analysis = root / "analysis"
    analysis.mkdir(parents=True, exist_ok=True)
    write_csv(analysis / "run_summary.csv", runs)
    write_csv(analysis / "condition_summary.csv", conditions)
    stage_rows = []
    for row in conditions:
        for stage in STAGES:
            stage_rows.append({
                "condition": row["condition"],
                "runs": row["runs"],
                "stage": stage,
                "mean_ms": row[f"{stage}_mean_ms"],
                "p95_ms": row[f"{stage}_p95_ms"],
            })
    write_csv(analysis / "stage_latency_summary.csv", stage_rows)
    comparisons = comparison_rows(conditions)
    if comparisons:
        write_csv(analysis / "baseline_vs_optimized.csv", comparisons)
    svg_latency(conditions, analysis / "latency_comparison.svg")
    svg_system(conditions, analysis / "system_comparison.svg")
    write_html(analysis / "final_report.html", conditions, comparisons, root)
    print(analysis)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
