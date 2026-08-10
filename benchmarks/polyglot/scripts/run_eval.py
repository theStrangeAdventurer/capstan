#!/usr/bin/env python3
"""Run a fixed, leak-resistant mini evaluation over Aider Polyglot tasks."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path


PINNED_COMMIT = "7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f"
SUITE = "mini-v2"
DEFAULT_TASKS = (
    "cpp/clock",
    "cpp/grade-school",
    "go/protein-translation",
    "go/transpose",
    "java/phone-number",
    "java/series",
    "javascript/promises",
    "javascript/triangle",
    "python/pov",
    "python/forth",
    "rust/acronym",
    "rust/word-count",
)

TEST_COMMANDS = {
    "cpp": (
        ("cmake", "-S", ".", "-B", "build"),
        ("cmake", "--build", "build", "-j2"),
        ("ctest", "--test-dir", "build", "--output-on-failure"),
    ),
    "go": (("go", "test", "./..."),),
    "java": (("./gradlew", "test", "--no-daemon", "--console", "plain"),),
    "javascript": (
        ("npm", "install", "--ignore-scripts", "--no-audit", "--no-fund"),
        ("npm", "test", "--", "--runInBand"),
    ),
    # Exercism's Python track uses unittest classes but names files *_test.py,
    # so the default unittest discovery pattern (test*.py) would miss them.
    # Use the standard library runner to keep the harness dependency-free.
    "python": (
        (sys.executable, "-m", "unittest", "discover", "-p", "*_test.py", "-v"),
    ),
    "rust": (("cargo", "test", "--quiet", "--", "--include-ignored"),),
}

REQUIRED_TOOLS = {
    "cpp": ("cmake",),
    "go": ("go",),
    "java": ("java",),
    "javascript": ("node", "npm"),
    "python": (),
    "rust": ("cargo",),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--agent-command",
        required=True,
        help="Command template containing {prompt_file} and {workdir}; {repo_root} is optional",
    )
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--test-timeout", type=int, default=300)
    parser.add_argument(
        "--tasks",
        help="Comma-separated language/exercise IDs; defaults to mini-v2",
    )
    parser.add_argument("--allow-corpus-drift", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def checked_output(args: list[str], cwd: Path) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def validate_corpus(corpus: Path, allow_drift: bool) -> str:
    if not (corpus / ".git").is_dir():
        raise RuntimeError(f"not a git checkout: {corpus}")
    status = checked_output(["git", "status", "--short"], corpus)
    if status:
        raise RuntimeError("corpus checkout is dirty; evaluation would not be reproducible")
    commit = checked_output(["git", "rev-parse", "HEAD"], corpus)
    if commit != PINNED_COMMIT and not allow_drift:
        raise RuntimeError(
            f"corpus is at {commit}, expected {PINNED_COMMIT}; "
            "use --allow-corpus-drift only for a deliberate update"
        )
    return commit


def task_source(corpus: Path, task_id: str) -> tuple[str, Path]:
    try:
        language, exercise = task_id.split("/", 1)
    except ValueError as exc:
        raise RuntimeError(f"invalid task ID: {task_id}") from exc
    source = corpus / language / "exercises" / "practice" / exercise
    if language not in TEST_COMMANDS or not source.is_dir():
        raise RuntimeError(f"unknown task: {task_id}")
    if not (source / ".docs" / "instructions.md").is_file():
        raise RuntimeError(f"missing public instructions: {task_id}")
    return language, source


def missing_toolchains(languages: set[str]) -> list[str]:
    return sorted(
        tool
        for language in languages
        for tool in REQUIRED_TOOLS[language]
        if shutil.which(tool) is None
    )


def validate_toolchains(languages: set[str]) -> None:
    missing = missing_toolchains(languages)
    if missing:
        raise RuntimeError("missing required toolchains: " + ", ".join(missing))


def public_prompt(source: Path) -> str:
    docs = source / ".docs"
    parts = [
        "Implement the exercise described below in the current working directory. "
        "You may inspect the project and run tests. Do not modify tests.\n",
        (docs / "instructions.md").read_text(encoding="utf-8").strip(),
    ]
    appendix = docs / "instructions.append.md"
    if appendix.is_file():
        text = appendix.read_text(encoding="utf-8").strip()
        if text:
            parts.append(text)
    return "\n\n".join(parts) + "\n"


def copy_task(source: Path, destination: Path) -> Path:
    # Some tracks (notably C++) derive source/test names from the exercise
    # directory basename. Preserve it instead of using a generic "work" name,
    # which makes CMake look for files such as work_test.cpp.
    work = destination / source.name
    shutil.copytree(source, work, ignore=shutil.ignore_patterns(".meta", ".docs"))
    return work


def task_config(source: Path) -> dict:
    config_path = source / ".meta" / "config.json"
    try:
        return json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"invalid task config: {config_path}") from exc


def configured_files(source: Path, kind: str) -> tuple[str, ...]:
    files = task_config(source).get("files", {}).get(kind, [])
    if not isinstance(files, list) or not all(isinstance(path, str) for path in files):
        raise RuntimeError(f"invalid {kind} file list in {source / '.meta/config.json'}")
    return tuple(files)


def make_scoring_copy(source: Path, agent_work: Path, destination: Path) -> Path:
    """Build a canonical test tree containing only the agent's solution edits."""
    score_work = copy_task(source, destination)
    for relative in configured_files(source, "solution"):
        agent_file = agent_work / relative
        score_file = score_work / relative
        if agent_file.is_file():
            score_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(agent_file, score_file)
        elif score_file.exists():
            score_file.unlink()
    return score_work


def enable_upstream_tests(language: str, source: Path, score_work: Path) -> None:
    """Match Aider's Polyglot harness treatment of disabled Exercism tests."""
    for relative in configured_files(source, "test"):
        test_file = score_work / relative
        if not test_file.is_file():
            raise RuntimeError(f"missing upstream test file: {test_file}")
        if language == "java" and test_file.suffix == ".java":
            content = test_file.read_text(encoding="utf-8")
            content = re.sub(r"^\s*@Disabled(?:\([^)]*\))?\s*\n", "", content, flags=re.MULTILINE)
            test_file.write_text(content, encoding="utf-8")
        elif language == "javascript" and test_file.name.endswith(".spec.js"):
            content = test_file.read_text(encoding="utf-8")
            test_file.write_text(re.sub(r"\bxtest\(", "test(", content), encoding="utf-8")


def _max_rss_bytes(max_rss: int | float) -> int:
    """Normalize wait4 ru_maxrss: bytes on macOS, KiB on Linux."""
    return int(max_rss if sys.platform == "darwin" else max_rss * 1024)


def _wait4(process: subprocess.Popen, timeout: int) -> tuple[int, bool, object]:
    """Wait for one process group leader and return its exact resource usage."""
    deadline = time.monotonic() + timeout
    timed_out = False
    termination_deadline = None
    while True:
        pid, status, usage = os.wait4(process.pid, os.WNOHANG)
        if pid == process.pid:
            return_code = os.waitstatus_to_exitcode(status)
            process.returncode = return_code
            return return_code, timed_out, usage

        now = time.monotonic()
        if not timed_out and now >= deadline:
            timed_out = True
            termination_deadline = now + 5
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        elif timed_out and termination_deadline is not None and now >= termination_deadline:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            termination_deadline = None
        time.sleep(0.05)


def run_process(command: list[str], cwd: Path, timeout: int, log_path: Path) -> dict:
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        log.write("$ " + shlex.join(command) + "\n\n")
        log.flush()
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        return_code, timed_out, usage = _wait4(process, timeout)
    max_rss_bytes = _max_rss_bytes(usage.ru_maxrss)
    resources = {
        "measurement": "wait4",
        "user_cpu_seconds": round(usage.ru_utime, 4),
        "system_cpu_seconds": round(usage.ru_stime, 4),
        "cpu_seconds": round(usage.ru_utime + usage.ru_stime, 4),
        "max_rss_bytes": max_rss_bytes,
        "max_rss_mib": round(max_rss_bytes / (1024 * 1024), 3),
    }
    return {
        "command": command,
        "return_code": return_code,
        "timed_out": timed_out,
        "seconds": round(time.monotonic() - started, 3),
        "log": str(log_path),
        "resources": resources,
    }


REPO_ROOT = Path(__file__).resolve().parents[3]


def render_agent_command(template: str, prompt_file: Path, work: Path) -> list[str]:
    if "{prompt_file}" not in template or "{workdir}" not in template:
        raise RuntimeError("--agent-command must contain {prompt_file} and {workdir}")
    rendered = template.replace("{repo_root}", shlex.quote(str(REPO_ROOT)))
    rendered = rendered.replace("{prompt_file}", shlex.quote(str(prompt_file))).replace(
        "{workdir}", shlex.quote(str(work))
    )
    return shlex.split(rendered)


def run_tests(
    language: str,
    source: Path,
    agent_work: Path,
    timeout: int,
    task_dir: Path,
) -> dict:
    score_root = task_dir / "score"
    score_work = make_scoring_copy(source, agent_work, score_root)
    enable_upstream_tests(language, source, score_work)
    steps = []
    for index, command in enumerate(TEST_COMMANDS[language], start=1):
        step = run_process(
            list(command), score_work, timeout, task_dir / f"test-{index}.log"
        )
        steps.append(step)
        if step["return_code"] != 0 or step["timed_out"]:
            break
    passed = len(steps) == len(TEST_COMMANDS[language]) and all(
        step["return_code"] == 0 and not step["timed_out"] for step in steps
    )
    return {"passed": passed, "steps": steps, "workdir": str(score_work)}


def classify(agent: dict, tests: dict) -> str:
    if agent["timed_out"]:
        return "agent_timeout"
    if tests["passed"]:
        return "passed"
    if agent["return_code"] != 0:
        return "agent_error"
    if any(step["timed_out"] for step in tests["steps"]):
        return "harness_error"
    return "test_failure"


def write_summary(output: Path, metadata: dict, results: list[dict]) -> None:
    passed = sum(result["status"] == "passed" for result in results)
    elapsed = sum(result["seconds"] for result in results)
    lines = [
        f"# Aider Polyglot {SUITE}",
        "",
        f"- Score: {passed}/{len(results)}",
        f"- Corpus commit: `{metadata['corpus_commit']}`",
        f"- Total task time: {elapsed:.1f}s",
        f"- Agent timeout: {metadata['agent_timeout']}s",
        "",
        "| Task | Status | Seconds | Agent CPU | Peak RSS |",
        "|---|---:|---:|---:|---:|",
    ]
    for result in results:
        lines.append(
            f"| `{result['task']}` | {result['status']} | {result['seconds']:.1f} "
            f"| {result['agent']['resources']['cpu_seconds']:.2f}s "
            f"| {result['agent']['resources']['max_rss_mib']:.1f} MiB |"
        )
    (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    corpus = args.corpus.expanduser().resolve()
    output = args.output.expanduser().resolve()
    task_ids = tuple(args.tasks.split(",")) if args.tasks else DEFAULT_TASKS
    commit = validate_corpus(corpus, args.allow_corpus_drift)

    if "{prompt_file}" not in args.agent_command or "{workdir}" not in args.agent_command:
        raise RuntimeError("--agent-command must contain {prompt_file} and {workdir}")

    plan = []
    for task_id in task_ids:
        language, source = task_source(corpus, task_id)
        plan.append({"task": task_id, "language": language, "source": str(source)})

    languages = {item["language"] for item in plan}

    if args.dry_run:
        print(
            json.dumps(
                {
                    "corpus_commit": commit,
                    "tasks": plan,
                    "missing_toolchains": missing_toolchains(languages),
                },
                indent=2,
            )
        )
        return 0

    validate_toolchains(languages)

    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    metadata = {
        "suite": SUITE,
        "corpus": str(corpus),
        "corpus_commit": commit,
        "tasks": list(task_ids),
        "agent_command": args.agent_command,
        "agent_timeout": args.timeout,
        "test_timeout": args.test_timeout,
        "comparable": commit == PINNED_COMMIT,
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )

    results = []
    for index, task_id in enumerate(task_ids, start=1):
        language, source = task_source(corpus, task_id)
        task_dir = output / "tasks" / task_id.replace("/", "--")
        task_dir.mkdir(parents=True)
        prompt_file = task_dir / "prompt.md"
        prompt_file.write_text(public_prompt(source), encoding="utf-8")
        work = copy_task(source, task_dir)
        command = render_agent_command(args.agent_command, prompt_file, work)
        print(f"[{index}/{len(task_ids)}] {task_id}", flush=True)
        agent = run_process(command, work, args.timeout, task_dir / "agent.log")
        tests = run_tests(language, source, work, args.test_timeout, task_dir)
        status = classify(agent, tests)
        result = {
            "task": task_id,
            "language": language,
            "status": status,
            "seconds": round(agent["seconds"] + sum(s["seconds"] for s in tests["steps"]), 3),
            "agent": agent,
            "tests": tests,
        }
        results.append(result)
        (output / "results.json").write_text(
            json.dumps(results, indent=2) + "\n", encoding="utf-8"
        )
        print(f"  {status} ({result['seconds']:.1f}s)", flush=True)

    write_summary(output, metadata, results)
    print(f"summary: {output / 'summary.md'}")
    return 0 if all(result["status"] == "passed" for result in results) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
