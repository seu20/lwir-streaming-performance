#!/usr/bin/env python3
"""Archive one PC/Raspberry Pi measurement run without deleting raw files."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import shlex
import shutil
import subprocess
import sys


RPI_FILES = (
    "capture_metrics.csv",
    "preprocess_metrics.csv",
    "streaming_metrics.csv",
    "encoding_metrics.csv",
    "fps_metrics.csv",
    "frame_metrics.csv",
    "network_metrics.csv",
)

PC_FILES = (
    "receiver_fps_metrics.csv",
    "receiver_frame_metrics.csv",
    "receiver_e2e_metrics.csv",
    "receiver_pts_trace.csv",
)


def command_output(command: list[str], cwd: pathlib.Path | None = None) -> str:
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Archive the current RPi and PC measurement CSV files."
    )
    parser.add_argument("--condition", required=True)
    parser.add_argument("--experiment", default="encoder-latency")
    parser.add_argument("--remote", default="pi@rpi.local")
    parser.add_argument(
        "--remote-repo",
        default="/home/pi/lwir-streaming-performance",
    )
    parser.add_argument("--warmup-frames", type=int, default=60)
    parser.add_argument("--source", default="dataset")
    parser.add_argument("--dataset", default="seq_001")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--encoder-tune", default="default")
    parser.add_argument("--speed-preset", default="default")
    parser.add_argument("--bframes", default="default")
    parser.add_argument("--bitrate-kbps", default="default")
    parser.add_argument("--notes", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo = pathlib.Path(__file__).resolve().parents[1]
    timestamp = dt.datetime.now().astimezone().strftime("%Y%m%dT%H%M%S%z")
    run_id = f"{timestamp}-run"
    run_dir = repo / "results" / args.experiment / args.condition / run_id
    pc_dir = run_dir / "pc"
    rpi_dir = run_dir / "rpi"
    pc_dir.mkdir(parents=True)
    rpi_dir.mkdir(parents=True)

    missing_pc = []
    for filename in PC_FILES:
        source = repo / filename
        if source.is_file():
            shutil.copy2(source, pc_dir / filename)
        else:
            missing_pc.append(filename)

    missing_rpi = []
    for filename in RPI_FILES:
        remote_path = f"{args.remote}:{args.remote_repo}/{filename}"
        result = subprocess.run(
            ["scp", "-q", remote_path, str(rpi_dir / filename)],
            check=False,
        )
        if result.returncode != 0:
            missing_rpi.append(filename)

    if missing_pc or missing_rpi:
        shutil.rmtree(run_dir)
        if missing_pc:
            print("Missing PC files: " + ", ".join(missing_pc), file=sys.stderr)
        if missing_rpi:
            print("Missing RPi files: " + ", ".join(missing_rpi), file=sys.stderr)
        return 1

    local_commit = command_output(
        ["git", "rev-parse", "--short", "HEAD"], cwd=repo
    )
    remote_command = (
        f"cd {shlex.quote(args.remote_repo)} && "
        "git rev-parse --short HEAD"
    )
    remote_commit = command_output(
        ["ssh", "-o", "BatchMode=yes", args.remote, remote_command]
    )

    manifest = {
        "experiment": args.experiment,
        "condition": args.condition,
        "run_id": run_id,
        "archived_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "warmup_frames": args.warmup_frames,
        "input": {
            "source": args.source,
            "dataset": args.dataset,
            "fps": args.fps,
            "width": args.width,
            "height": args.height,
        },
        "encoder": {
            "tune": args.encoder_tune,
            "speed_preset": args.speed_preset,
            "bframes": args.bframes,
            "bitrate_kbps": args.bitrate_kbps,
        },
        "network": {
            "transport": "RTP/UDP",
            "receiver_ip": "10.42.0.1",
            "port": 5004,
        },
        "git": {
            "pc": local_commit,
            "rpi": remote_commit,
        },
        "notes": args.notes,
        "files": {
            "pc": list(PC_FILES),
            "rpi": list(RPI_FILES),
        },
    }
    (run_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    print(run_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
