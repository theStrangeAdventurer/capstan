#!/usr/bin/env python3
"""Run one eval task through OpenCode without changing its public prompt."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prompt-file", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--variant")
    parser.add_argument("--reasoning-effort", choices=("minimal", "low", "medium", "high", "xhigh"))
    parser.add_argument("--provider-sort", choices=("price", "throughput", "latency"))
    parser.add_argument("--upstream", help="OpenRouter upstream slug; disables fallback")
    return parser.parse_args()


def reasoning_override(model: str, effort: str | None, provider_sort: str | None = None,
                       upstream: str | None = None) -> str:
    """Return an OpenCode config patch that maps an OpenRouter effort to its request body."""
    provider, separator, model_id = model.partition("/")
    if provider != "openrouter" or not separator or not model_id:
        raise ValueError(
            "reasoning/routing overrides require an OpenRouter model in the form "
            "openrouter/<publisher>/<model>"
        )
    model_config = {}
    if effort:
        model_config["variants"] = {effort: {"reasoning": {"effort": effort}}}
    routing = {}
    if provider_sort:
        routing["sort"] = provider_sort
    if upstream:
        routing.update(only=[upstream], allow_fallbacks=False)
    if routing:
        model_config["options"] = {"provider": routing}
    return json.dumps(
        {
            "provider": {
                "openrouter": {
                    "models": {
                        model_id: model_config,
                    },
                },
            },
        },
        separators=(",", ":"),
    )


def main() -> int:
    args = parse_args()
    prompt_file = Path(args.prompt_file).resolve()
    workdir = Path(args.workdir).resolve()
    if not prompt_file.is_file():
        raise SystemExit(f"missing prompt file: {prompt_file}")
    if not workdir.is_dir():
        raise SystemExit(f"missing work directory: {workdir}")

    opencode = shutil.which("opencode")
    if not opencode:
        raise SystemExit("opencode is not installed or not on PATH")

    command = [
        opencode,
        "run",
        "--pure",
        "--model",
        args.model,
        "--agent",
        "build",
        "--format",
        "json",
        "--dir",
        str(workdir),
        "--dangerously-skip-permissions",
    ]
    if args.reasoning_effort:
        command.extend(["--variant", args.reasoning_effort])
    elif args.variant:
        command.extend(["--variant", args.variant])
    command.append(prompt_file.read_text(encoding="utf-8"))

    # --pure disables external plugins only. Isolate every other extension
    # surface explicitly while retaining provider auth stored outside config.
    with tempfile.TemporaryDirectory(prefix="opencode-eval-config-") as config_dir:
        env = os.environ.copy()
        env.update(
            {
                "OPENCODE_CONFIG_DIR": config_dir,
                "OPENCODE_DISABLE_PROJECT_CONFIG": "1",
                "OPENCODE_DISABLE_EXTERNAL_SKILLS": "1",
                "OPENCODE_DISABLE_CLAUDE_CODE": "1",
                "OPENCODE_PERMISSION": '{"skill":"deny"}',
            }
        )
        if args.reasoning_effort or args.provider_sort or args.upstream:
            env["OPENCODE_CONFIG_CONTENT"] = reasoning_override(
                args.model, args.reasoning_effort, args.provider_sort, args.upstream)
        process = subprocess.Popen(
            command,
            cwd=workdir,
            env=env,
            stdin=subprocess.DEVNULL,
        )
        pid_file = os.environ.get("CAPSTAN_BENCH_AGENT_PID_FILE")
        if pid_file:
            Path(pid_file).write_text(f"{process.pid}\n", encoding="ascii")
        return process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
