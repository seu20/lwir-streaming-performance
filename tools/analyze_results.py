#!/usr/bin/env python3
"""Create a compact CSV and HTML report from archived experiment runs."""

from __future__ import annotations

import argparse
import csv
import html
import json
import math
import pathlib
import statistics


STAGE_FILES = {
    "capture": ("rpi/capture_metrics.csv", "processing_ms"),
    "preprocess": ("rpi/preprocess_metrics.csv", "processing_ms"),
    "streaming_submit": ("rpi/streaming_metrics.csv", "processing_ms"),
    "encoding": ("rpi/encoding_metrics.csv", "encoding_ms"),
    "e2e": ("pc/receiver_e2e_metrics.csv", "e2e_ms"),
}


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (
        position - lower
    )


def read_rows(path: pathlib.Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def summarize(values: list[float]) -> dict[str, float | int]:
    return {
        "count": len(values),
        "mean_ms": statistics.fmean(values),
        "median_ms": percentile(values, 0.50),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
        "max_ms": max(values),
    }


def active_receiver_fps(run_dir: pathlib.Path) -> float | None:
    rows = read_rows(run_dir / "pc/receiver_fps_metrics.csv")
    active = [row for row in rows if int(row["frame_count"]) > 0]
    if len(active) > 2:
        active = active[1:-1]
    if not active:
        return None
    frames = sum(int(row["frame_count"]) for row in active)
    duration = sum(
        float(row["interval_end_s"]) - float(row["interval_start_s"])
        for row in active
    )
    return frames / duration if duration > 0 else None


def frame_counts(run_dir: pathlib.Path) -> tuple[int | None, int | None]:
    sender = read_rows(run_dir / "rpi/frame_metrics.csv")
    receiver = read_rows(run_dir / "pc/receiver_frame_metrics.csv")
    sent = int(sender[0]["streaming_submit_success_count"]) if sender else None
    decoded = int(receiver[0]["decoded_frame_count"]) if receiver else None
    return sent, decoded


def analyze_run(manifest_path: pathlib.Path) -> list[dict[str, object]]:
    run_dir = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    warmup = int(manifest.get("warmup_frames", 0))
    receiver_fps = active_receiver_fps(run_dir)
    sent, decoded = frame_counts(run_dir)
    records = []

    for metric, (relative_path, column) in STAGE_FILES.items():
        rows = read_rows(run_dir / relative_path)
        values = []
        for row in rows:
            if row.get("frame_id") and int(row["frame_id"]) <= warmup:
                continue
            raw = row.get(column, "")
            if raw:
                values.append(float(raw))
        if not values:
            continue
        record = {
            "experiment": manifest["experiment"],
            "condition": manifest["condition"],
            "run_id": manifest["run_id"],
            "metric": metric,
            **summarize(values),
            "receiver_active_fps": receiver_fps,
            "sent_frames": sent,
            "decoded_frames": decoded,
            "count_gap": sent - decoded if sent is not None and decoded is not None else None,
        }
        records.append(record)
    return records


def format_value(value: object, digits: int = 3) -> str:
    if value is None:
        return "n.a."
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def write_html(records: list[dict[str, object]], path: pathlib.Path) -> None:
    latency_records = [record for record in records if record["metric"] != "e2e"]
    max_p95 = max(float(record["p95_ms"]) for record in latency_records)
    bars = []
    for index, record in enumerate(latency_records):
        y = 28 + index * 30
        width = 650 * float(record["p95_ms"]) / max_p95
        label = f"{record['condition']} / {record['metric']}"
        bars.append(
            f'<text x="0" y="{y + 14}" font-size="12">{html.escape(label)}</text>'
            f'<rect x="220" y="{y}" width="{width:.2f}" height="18" fill="#4f46e5" />'
            f'<text x="{230 + width:.2f}" y="{y + 14}" font-size="12">'
            f'{float(record["p95_ms"]):.3f} ms</text>'
        )
    chart_height = 55 + 30 * len(latency_records)
    rows = []
    for record in records:
        rows.append(
            "<tr>"
            + "".join(
                f"<td>{html.escape(format_value(record[key]))}</td>"
                for key in (
                    "condition",
                    "run_id",
                    "metric",
                    "mean_ms",
                    "median_ms",
                    "p95_ms",
                    "p99_ms",
                    "max_ms",
                    "receiver_active_fps",
                    "sent_frames",
                    "decoded_frames",
                    "count_gap",
                )
            )
            + "</tr>"
        )
    document = f"""<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Video pipeline experiment report</title>
<style>
body {{ font-family: Arial, sans-serif; margin: 32px; color: #111827; }}
h1, h2 {{ font-weight: 600; }}
svg {{ width: 100%; max-width: 1000px; height: auto; border: 1px solid #d1d5db; }}
table {{ border-collapse: collapse; font-size: 13px; }}
th, td {{ border-bottom: 1px solid #d1d5db; padding: 7px 9px; text-align: right; }}
th:first-child, td:first-child, th:nth-child(2), td:nth-child(2), th:nth-child(3), td:nth-child(3) {{ text-align: left; }}
th {{ background: #f3f4f6; }}
</style>
</head>
<body>
<h1>Video pipeline experiment report</h1>
<h2>P95 latency by stage</h2>
<svg viewBox="0 0 1000 {chart_height}" role="img" aria-label="P95 latency by condition and stage">
{''.join(bars)}
</svg>
<h2>Run summary</h2>
<table>
<thead><tr><th>Condition</th><th>Run</th><th>Metric</th><th>Mean ms</th><th>Median ms</th><th>P95 ms</th><th>P99 ms</th><th>Max ms</th><th>Receiver FPS</th><th>Sent</th><th>Decoded</th><th>Gap</th></tr></thead>
<tbody>{''.join(rows)}</tbody>
</table>
</body>
</html>
"""
    path.write_text(document, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("results_root", type=pathlib.Path)
    args = parser.parse_args()
    root = args.results_root.resolve()
    manifests = sorted(root.rglob("manifest.json"))
    if not manifests:
        parser.error(f"No manifest.json found under {root}")

    records = []
    for manifest in manifests:
        records.extend(analyze_run(manifest))

    analysis_dir = root / "analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)
    fieldnames = list(records[0])
    with (analysis_dir / "summary.csv").open(
        "w", newline="", encoding="utf-8"
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(records)
    write_html(records, analysis_dir / "report.html")
    print(analysis_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
