#!/usr/bin/env python3
"""Run one experiment condition repeatedly on the PC and Raspberry Pi."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import shlex
import subprocess
import sys
import time


def output(command: list[str], cwd: pathlib.Path | None = None) -> str:
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--condition", required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument(
        "--campaign",
        default=dt.datetime.now().strftime("%Y%m%d-baseline-study"),
    )
    parser.add_argument("--remote", default="pi@rpi.local")
    parser.add_argument(
        "--remote-repo",
        default="/home/pi/lwir-streaming-performance",
    )
    parser.add_argument(
        "--dataset-path",
        default="/workspace/dataset/seq_001/images",
    )
    parser.add_argument("--host", default="10.42.0.1")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--warmup-frames", type=int, default=60)
    parser.add_argument("--record", action="store_true")
    parser.add_argument(
        "--deploy",
        action="store_true",
        help="Sync the current RPi source and rebuild before running.",
    )
    return parser.parse_args()


def deploy(args: argparse.Namespace, repo: pathlib.Path) -> None:
    subprocess.run(
        [
            "rsync",
            "-a",
            "--exclude",
            "build/",
            f"{repo / 'rpi'}/",
            f"{args.remote}:{args.remote_repo}/rpi/",
        ],
        check=True,
    )
    remote_build = (
        f"cd {shlex.quote(args.remote_repo)} && "
        "cmake --build build -j2"
    )
    subprocess.run(
        [
            "ssh",
            "-o",
            "BatchMode=yes",
            args.remote,
            remote_build,
        ],
        check=True,
    )


def run_once(
    args: argparse.Namespace,
    repo: pathlib.Path,
    run_index: int,
) -> pathlib.Path:
    run_name = f"run-{run_index:02d}"
    run_dir = (
        repo
        / "results"
        / "lwir-performance"
        / args.campaign
        / args.condition
        / run_name
    )
    if run_dir.exists():
        raise RuntimeError(f"Run directory already exists: {run_dir}")

    pc_dir = run_dir / "pc"
    rpi_dir = run_dir / "rpi"
    pc_dir.mkdir(parents=True)
    rpi_dir.mkdir(parents=True)

    remote_run_dir = (
        f"{args.remote_repo}/results/lwir-performance/"
        f"{args.campaign}/{args.condition}/{run_name}/rpi"
    )
    remote_setup = f"mkdir -p {shlex.quote(remote_run_dir)}"
    subprocess.run(
        ["ssh", "-o", "BatchMode=yes", args.remote, remote_setup],
        check=True,
    )

    receiver_command = [
        "/usr/bin/time",
        "-v",
        "-o",
        str(pc_dir / "resource_usage.txt"),
        str(repo / "pc/build/lwir_receiver"),
        "--clocks-synchronized",
        "--condition",
        args.condition,
        "--output-dir",
        str(pc_dir),
        "--idle-timeout",
        "3",
    ]
    if args.record and run_index == 1:
        receiver_command.extend(
            ["--record-output", str(pc_dir / f"{args.condition}.mp4")]
        )

    remote_program = [
        "/usr/bin/time",
        "-v",
        "-o",
        f"{remote_run_dir}/resource_usage.txt",
        f"{args.remote_repo}/build/video_pipeline",
        "--condition",
        args.condition,
        "--dataset-path",
        args.dataset_path,
        "--host",
        args.host,
        "--fps",
        str(args.fps),
        "--output-dir",
        remote_run_dir,
    ]
    remote_command = " ".join(shlex.quote(part) for part in remote_program)

    started_at = dt.datetime.now().astimezone()
    with (pc_dir / "receiver.log").open("w", encoding="utf-8") as pc_log:
        receiver = subprocess.Popen(
            receiver_command,
            cwd=repo,
            stdout=pc_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        time.sleep(1.0)

        with (rpi_dir / "ssh_runner.log").open("w", encoding="utf-8") as ssh_log:
            sender = subprocess.Popen(
                [
                    "ssh",
                    "-o",
                    "BatchMode=yes",
                    args.remote,
                    remote_command,
                ],
                stdout=ssh_log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            sender_code = sender.wait(timeout=180)

        try:
            receiver_code = receiver.wait(timeout=15)
        except subprocess.TimeoutExpired:
            receiver.terminate()
            receiver_code = receiver.wait(timeout=5)

    subprocess.run(
        [
            "rsync",
            "-a",
            f"{args.remote}:{remote_run_dir}/",
            f"{rpi_dir}/",
        ],
        check=True,
    )

    finished_at = dt.datetime.now().astimezone()
    manifest = {
        "experiment": "lwir-performance",
        "campaign": args.campaign,
        "condition": args.condition,
        "run_id": run_name,
        "started_at": started_at.isoformat(timespec="seconds"),
        "finished_at": finished_at.isoformat(timespec="seconds"),
        "warmup_frames": args.warmup_frames,
        "input": {
            "source": "dataset",
            "dataset_path": args.dataset_path,
            "frame_count": 971,
            "format": "16-bit PNG",
            "width": 640,
            "height": 480,
            "fps": args.fps,
        },
        "network": {
            "transport": "RTP/UDP",
            "receiver_ip": args.host,
            "port": 5004,
        },
        "git": {
            "pc": output(["git", "rev-parse", "--short", "HEAD"], repo),
            "rpi": output(
                [
                    "ssh",
                    "-o",
                    "BatchMode=yes",
                    args.remote,
                    f"cd {shlex.quote(args.remote_repo)} && git rev-parse --short HEAD",
                ]
            ),
        },
        "exit_codes": {
            "pc_receiver": receiver_code,
            "rpi_sender": sender_code,
        },
        "recording_enabled": args.record and run_index == 1,
    }
    (run_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    if sender_code != 0 or receiver_code != 0:
        raise RuntimeError(
            f"Condition {args.condition} {run_name} failed: "
            f"sender={sender_code}, receiver={receiver_code}"
        )
    return run_dir


def main() -> int:
    args = parse_args()
    if args.repeat < 1:
        raise SystemExit("--repeat must be at least 1")
    if args.repeat > 3:
        raise SystemExit("--repeat must not exceed 3 for the final experiment")
    repo = pathlib.Path(__file__).resolve().parents[1]

    if args.deploy:
        deploy(args, repo)

    for run_index in range(1, args.repeat + 1):
        print(
            f"[{run_index}/{args.repeat}] {args.condition}",
            flush=True,
        )
        try:
            run_dir = run_once(args, repo, run_index)
        except Exception as error:
            print(str(error), file=sys.stderr)
            return 1
        print(run_dir, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
