"""Promptfoo providers for the canonical Aider Polyglot agent benchmark."""

from __future__ import annotations

import importlib.util
import json
import re
from pathlib import Path
from types import ModuleType
from typing import Any


def _load_harness() -> ModuleType:
    path = Path(__file__).resolve().parents[1] / "scripts" / "run_eval.py"
    spec = importlib.util.spec_from_file_location("aider_polyglot_harness", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load benchmark harness: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HARNESS = _load_harness()


def _slug(value: Any) -> str:
    text = re.sub(r"[^A-Za-z0-9._-]+", "-", str(value or "unknown"))
    return text.strip("-") or "unknown"


def _required_config(options: dict, name: str) -> Any:
    config = options.get("config") or {}
    value = config.get(name)
    if value in (None, ""):
        raise RuntimeError(f"provider config requires {name}")
    return value


def _run(prompt: str, options: dict, context: dict) -> dict:
    config = options.get("config") or {}
    task_id = str((context.get("vars") or {}).get("task", ""))
    if not task_id:
        return {"output": "", "error": "test case is missing vars.task"}

    try:
        corpus = Path(_required_config(options, "corpus")).expanduser().resolve()
        output_root = Path(_required_config(options, "output_root")).expanduser().resolve()
        agent_command = str(_required_config(options, "agent_command"))
        agent_timeout = int(config.get("agent_timeout", 180))
        test_timeout = int(config.get("test_timeout", 300))

        corpus_commit = HARNESS.validate_corpus(corpus, False)
        language, source = HARNESS.task_source(corpus, task_id)
        HARNESS.validate_toolchains({language})

        provider_name = config.get("provider_name", options.get("id", "provider"))
        evaluation_id = context.get("evaluationId", "eval")
        repeat_index = int(context.get("repeatIndex", 0))
        task_dir = (
            output_root
            / _slug(evaluation_id)
            / _slug(provider_name)
            / f"r{repeat_index + 1}"
            / _slug(task_id)
        )
        if task_dir.exists() and any(task_dir.iterdir()):
            raise RuntimeError(f"task output directory is not empty: {task_dir}")
        task_dir.mkdir(parents=True, exist_ok=True)

        prompt_file = task_dir / "prompt.md"
        prompt_file.write_text(prompt, encoding="utf-8")
        work = HARNESS.copy_task(source, task_dir / "agent")
        command = HARNESS.render_agent_command(agent_command, prompt_file, work)
        agent = HARNESS.run_process(
            command, work, agent_timeout, task_dir / "agent.log"
        )
        tests = HARNESS.run_tests(
            language, source, work, test_timeout, task_dir
        )
        status = HARNESS.classify(agent, tests)
        seconds = round(
            agent["seconds"] + sum(step["seconds"] for step in tests["steps"]), 3
        )
        result = {
            "task": task_id,
            "language": language,
            "status": status,
            "seconds": seconds,
            "provider": provider_name,
            "model": config.get("model"),
            "corpus_commit": corpus_commit,
            "repeat": repeat_index + 1,
            "agent": {
                "return_code": agent["return_code"],
                "timed_out": agent["timed_out"],
                "seconds": agent["seconds"],
                "log": agent["log"],
                "resources": agent["resources"],
            },
            "tests": {
                "passed": tests["passed"],
                "workdir": tests["workdir"],
                "steps": tests["steps"],
            },
        }
        result_path = task_dir / "result.json"
        result_path.write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )
        return {
            "output": json.dumps(result, indent=2),
            "metadata": {
                "task": task_id,
                "language": language,
                "status": status,
                "repeat": repeat_index + 1,
                "result_path": str(result_path),
                "agent_log": agent["log"],
                "agent_cpu_seconds": agent["resources"]["cpu_seconds"],
                "agent_max_rss_mib": agent["resources"]["max_rss_mib"],
                "resource_measurement": agent["resources"]["measurement"],
            },
            "latencyMs": int(seconds * 1000),
            "cached": False,
        }
    except Exception as exc:
        return {
            "output": json.dumps(
                {"task": task_id, "status": "harness_error", "error": str(exc)}
            ),
            "error": f"{type(exc).__name__}: {exc}",
            "cached": False,
        }


def call_capstan(prompt: str, options: dict, context: dict) -> dict:
    return _run(prompt, options, context)


def call_opencode(prompt: str, options: dict, context: dict) -> dict:
    return _run(prompt, options, context)
