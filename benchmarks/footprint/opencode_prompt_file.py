#!/usr/bin/env python3
"""Run opencode with a prompt loaded from a file without shell re-parsing."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default="opencode")
    parser.add_argument("--model", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--prompt-file", required=True)
    parser.add_argument("--dangerously-skip-permissions", action="store_true")
    args = parser.parse_args()

    prompt = Path(args.prompt_file).read_text()
    cmd = [
        args.bin,
        "run",
        "--dir",
        args.workdir,
        "-m",
        args.model,
    ]
    if args.dangerously_skip_permissions:
        cmd.append("--dangerously-skip-permissions")
    cmd.append(prompt)
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
