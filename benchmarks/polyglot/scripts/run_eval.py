#!/usr/bin/env python3
"""Run a fixed, leak-resistant mini evaluation over Aider Polyglot tasks."""

from __future__ import annotations

import argparse
import ctypes
import json
import math
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
    parser.add_argument(
        "--replicate-id",
        help="Stable repetition ID shared by agents, for example r1, r2, or r3",
    )
    parser.add_argument(
        "--comparison-id",
        help="Shared provider/model/reasoning configuration ID for paired agent comparisons",
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def inferred_replicate_id(output: Path) -> str | None:
    match = re.search(r"(?:^|[-_])(r[0-9]+)$", output.name, re.IGNORECASE)
    return match.group(1).lower() if match else None


def selected_task_ids(value: str | None) -> tuple[str, ...]:
    if value is None:
        return DEFAULT_TASKS
    task_ids = tuple(part.strip() for part in value.split(","))
    if any(not task_id for task_id in task_ids):
        raise RuntimeError("--tasks must contain non-empty task IDs")
    if len(set(task_ids)) != len(task_ids):
        raise RuntimeError("--tasks must not contain duplicates")
    return task_ids


def validate_timeout(name: str, value: int) -> int:
    if value <= 0:
        raise RuntimeError(f"{name} must be greater than zero")
    return value


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


class _DarwinTaskInfo(ctypes.Structure):
    _fields_ = [
        ("virtual_size", ctypes.c_uint64),
        ("resident_size", ctypes.c_uint64),
        ("total_user", ctypes.c_uint64),
        ("total_system", ctypes.c_uint64),
        ("threads_user", ctypes.c_uint64),
        ("threads_system", ctypes.c_uint64),
        ("policy", ctypes.c_int32),
        ("faults", ctypes.c_int32),
        ("pageins", ctypes.c_int32),
        ("cow_faults", ctypes.c_int32),
        ("messages_sent", ctypes.c_int32),
        ("messages_received", ctypes.c_int32),
        ("syscalls_mach", ctypes.c_int32),
        ("syscalls_unix", ctypes.c_int32),
        ("context_switches", ctypes.c_int32),
        ("thread_count", ctypes.c_int32),
        ("running_threads", ctypes.c_int32),
        ("priority", ctypes.c_int32),
    ]


_PROC_PIDINFO = None


def _darwin_proc_pidinfo():
    global _PROC_PIDINFO
    if _PROC_PIDINFO is None:
        proc_pidinfo = ctypes.CDLL("/usr/lib/libproc.dylib").proc_pidinfo
        proc_pidinfo.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_uint64,
            ctypes.c_void_p,
            ctypes.c_int,
        ]
        proc_pidinfo.restype = ctypes.c_int
        _PROC_PIDINFO = proc_pidinfo
    return _PROC_PIDINFO


def _pid_rss_bytes(pid: int) -> int | None:
    """Return current RSS for exactly one PID, excluding all descendants."""
    if sys.platform == "darwin":
        info = _DarwinTaskInfo()
        size = ctypes.sizeof(info)
        if _darwin_proc_pidinfo()(pid, 4, 0, ctypes.byref(info), size) != size:
            return None
        return int(info.resident_size)
    if sys.platform.startswith("linux"):
        try:
            status = Path(f"/proc/{pid}/status").read_text(encoding="ascii")
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            return None
        match = re.search(r"^VmRSS:\s+(\d+)\s+kB$", status, re.MULTILINE)
        return int(match.group(1)) * 1024 if match else None
    return None


def _reported_agent_pid(pid_file: Path, default_pid: int) -> int:
    try:
        value = int(pid_file.read_text(encoding="ascii").strip())
        return value if value > 0 else default_pid
    except (FileNotFoundError, ValueError, OSError):
        return default_pid


def _wait4(
    process: subprocess.Popen, timeout: int, agent_pid_file: Path
) -> tuple[int, bool, object, int]:
    """Wait for a process group and sample only its reported primary agent PID."""
    deadline = time.monotonic() + timeout
    timed_out = False
    termination_deadline = None
    agent_peak_rss_bytes = 0
    while True:
        agent_pid = _reported_agent_pid(agent_pid_file, process.pid)
        rss_bytes = _pid_rss_bytes(agent_pid)
        if rss_bytes is not None:
            agent_peak_rss_bytes = max(agent_peak_rss_bytes, rss_bytes)

        pid, status, usage = os.wait4(process.pid, os.WNOHANG)
        if pid == process.pid:
            return_code = os.waitstatus_to_exitcode(status)
            process.returncode = return_code
            return return_code, timed_out, usage, agent_peak_rss_bytes

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
    agent_pid_file = log_path.with_name(log_path.stem + "-primary.pid")
    agent_pid_file.unlink(missing_ok=True)
    env = os.environ.copy()
    env["CAPSTAN_BENCH_AGENT_PID_FILE"] = str(agent_pid_file)
    try:
        with log_path.open("w", encoding="utf-8") as log:
            log.write("$ " + shlex.join(command) + "\n\n")
            log.flush()
            process = subprocess.Popen(
                command,
                cwd=cwd,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
            return_code, timed_out, usage, agent_peak_rss_bytes = _wait4(
                process, timeout, agent_pid_file
            )
    finally:
        agent_pid_file.unlink(missing_ok=True)
    wait4_max_rss_bytes = _max_rss_bytes(usage.ru_maxrss)
    resources = {
        "measurement": "primary_pid_sample_50ms",
        "user_cpu_seconds": round(usage.ru_utime, 4),
        "system_cpu_seconds": round(usage.ru_stime, 4),
        "cpu_seconds": round(usage.ru_utime + usage.ru_stime, 4),
        "max_rss_bytes": agent_peak_rss_bytes,
        "max_rss_mib": round(agent_peak_rss_bytes / (1024 * 1024), 3),
        "wait4_max_rss_bytes": wait4_max_rss_bytes,
        "wait4_max_rss_mib": round(wait4_max_rss_bytes / (1024 * 1024), 3),
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


def _plain_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _nonnegative_number(value: object) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and value >= 0)


def _trace_error(status: str, source: Path, message: str, **details) -> dict:
    return {
        "status": status,
        "complete": False,
        "partial": status == "partial",
        "source": str(source),
        "error": message,
        **details,
    }


def _validate_terminal_data(data: dict) -> None:
    if "ok" in data and not isinstance(data["ok"], bool):
        raise ValueError("terminal ok must be boolean")
    if "intended_exit_code" in data and not _plain_int(data["intended_exit_code"]):
        raise ValueError("terminal intended_exit_code must be an integer")
    if "turns" in data and (not _plain_int(data["turns"]) or data["turns"] < 0):
        raise ValueError("terminal turns must be a non-negative integer")
    if "duration_ms" in data and not _nonnegative_number(data["duration_ms"]):
        raise ValueError("terminal duration_ms must be non-negative")

    counts = data.get("counts")
    if counts is not None:
        if not isinstance(counts, dict):
            raise ValueError("terminal counts must be an object")
        for key, value in counts.items():
            if not _plain_int(value) or value < 0:
                raise ValueError(f"terminal counts.{key} must be a non-negative integer")

    for container_name in ("timing", "breakdown"):
        container = data.get(container_name)
        if container is None:
            continue
        if not isinstance(container, dict):
            raise ValueError(f"terminal {container_name} must be an object")
        for key, value in container.items():
            if key == "conservation_error_ms":
                if (not isinstance(value, (int, float)) or isinstance(value, bool)
                        or not math.isfinite(value)):
                    raise ValueError("conservation error must be finite numeric")
            elif not _nonnegative_number(value):
                raise ValueError(f"terminal {container_name}.{key} must be non-negative")


def _validate_event_lifecycle(events: list[dict]) -> None:
    if events[0]["event"] != "run.started":
        raise ValueError("trace must start with run.started")
    if sum(event["event"] == "run.started" for event in events) != 1:
        raise ValueError("trace must contain exactly one run.started")

    active_kind = None
    active_identity = None
    for event in events:
        name = event["event"]
        data = event["data"]
        if name in ("model.request.started", "model.request.finished"):
            turn = data.get("turn")
            attempt = data.get("attempt")
            if (not _plain_int(turn) or turn < 0 or
                    not _plain_int(attempt) or attempt < 0):
                raise ValueError("model lifecycle event has invalid identity")
            identity = (turn, attempt)
            if name.endswith(".started"):
                if active_kind is not None:
                    raise ValueError("overlapping model/tool lifecycle events")
                active_kind = "model"
                active_identity = identity
            elif active_kind != "model" or active_identity != identity:
                raise ValueError("unmatched model.request.finished event")
            else:
                active_kind = None
                active_identity = None
        elif name in ("tool.started", "tool.finished"):
            tool_name = data.get("name")
            if not isinstance(tool_name, str) or not tool_name:
                raise ValueError("tool lifecycle event has invalid identity")
            if name.endswith(".started"):
                if active_kind is not None:
                    raise ValueError("overlapping model/tool lifecycle events")
                active_kind = "tool"
                active_identity = tool_name
            elif active_kind != "tool" or active_identity != tool_name:
                raise ValueError("unmatched tool.finished event")
            else:
                active_kind = None
                active_identity = None
    if active_kind is not None:
        raise ValueError(f"unfinished {active_kind} lifecycle event")


def load_trace_summary(path: Path) -> dict | None:
    partial_path = Path(str(path) + ".partial")
    source = partial_path if partial_path.is_file() else path
    if not source.is_file():
        return None
    is_partial = source == partial_path
    try:
        content = source.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return _trace_error("corrupt", source, "trace is not readable UTF-8")

    truncated_tail = bool(content) and not content.endswith("\n")
    if truncated_tail:
        if not is_partial:
            return _trace_error("corrupt", source,
                                "published trace has an unterminated record")
        content = content.rsplit("\n", 1)[0] + ("\n" if "\n" in content else "")

    events = []
    try:
        for line in content.splitlines():
            event = json.loads(line)
            if (not isinstance(event, dict)
                    or event.get("schema") != "capstan.trace.v1"
                    or not _plain_int(event.get("seq"))
                    or not _nonnegative_number(event.get("timestamp_ms"))
                    or not _nonnegative_number(event.get("elapsed_ms"))
                    or not isinstance(event.get("run_id"), str)
                    or not event.get("run_id")
                    or not isinstance(event.get("event"), str)
                    or not event.get("event")
                    or not isinstance(event.get("data"), dict)):
                raise ValueError("trace event has an invalid schema or field type")
            events.append(event)
    except (json.JSONDecodeError, ValueError, TypeError):
        return _trace_error("corrupt", source, "invalid trace event")

    if not events:
        if is_partial:
            return _trace_error("partial", source, "trace was not finalized",
                                observed_events=0,
                                truncated_tail=truncated_tail)
        return _trace_error("corrupt", source, "empty published trace")

    run_ids = {event["run_id"] for event in events}
    sequences = [event["seq"] for event in events]
    elapsed = [event["elapsed_ms"] for event in events]
    if (len(run_ids) != 1 or sequences != list(range(1, len(events) + 1))
            or elapsed != sorted(elapsed)):
        return _trace_error("corrupt", source,
                            "invalid trace sequence, run ID, or elapsed time")

    terminals = [event for event in events if event["event"] == "run.finished"]
    if is_partial:
        return _trace_error(
            "partial", source, "trace was not finalized",
            observed_events=len(events), last_seq=events[-1]["seq"],
            last_elapsed_ms=events[-1]["elapsed_ms"],
            truncated_tail=truncated_tail,
        )
    if len(terminals) != 1 or events[-1] is not terminals[0]:
        return _trace_error("corrupt", source,
                            "published trace lacks one final terminal event")

    terminal = terminals[0]["data"]
    try:
        _validate_event_lifecycle(events)
        _validate_terminal_data(terminal)
        counts = terminal.get("counts", {})
        expected_models = counts.get("model_requests")
        expected_tools = counts.get("tool_calls")
        finished_models = sum(event["event"] == "model.request.finished"
                              for event in events)
        finished_tools = sum(event["event"] == "tool.finished"
                             for event in events)
        if expected_models is not None and expected_models != finished_models:
            raise ValueError("model request count does not match events")
        if expected_tools is not None and expected_tools != finished_tools:
            raise ValueError("tool call count does not match events")
    except (ValueError, TypeError):
        return _trace_error("corrupt", source, "invalid terminal trace summary")

    return {
        "status": "complete",
        "complete": True,
        "partial": False,
        "source": str(source),
        "observed_events": len(events),
        "run_id": events[0]["run_id"],
        "terminal": terminal,
    }


def reconcile_process_telemetry(
    telemetry: dict | None, *, timed_out: bool, return_code: int
) -> dict | None:
    if not isinstance(telemetry, dict) or telemetry.get("complete") is not True:
        return telemetry
    terminal = telemetry.get("terminal")
    reason = None
    if timed_out:
        reason = "process timed out after publishing a terminal trace"
    elif not isinstance(terminal, dict):
        reason = "complete trace has no terminal payload"
    else:
        expected_ok = return_code == 0
        if terminal.get("ok") is not expected_ok:
            reason = "terminal ok does not match process return code"
        elif terminal.get("intended_exit_code") != return_code:
            reason = "terminal intended_exit_code does not match process return code"
    if reason is None:
        return telemetry

    inconsistent = dict(telemetry)
    inconsistent.update({
        "status": "inconsistent",
        "complete": False,
        "partial": False,
        "error": reason,
    })
    return inconsistent


def render_agent_command(
    template: str, prompt_file: Path, work: Path, trace_file: Path
) -> list[str]:
    if "{prompt_file}" not in template or "{workdir}" not in template:
        raise RuntimeError("--agent-command must contain {prompt_file} and {workdir}")
    rendered = template.replace("{repo_root}", shlex.quote(str(REPO_ROOT)))
    rendered = rendered.replace("{prompt_file}", shlex.quote(str(prompt_file))).replace(
        "{workdir}", shlex.quote(str(work))
    ).replace("{trace_file}", shlex.quote(str(trace_file)))
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
    if agent["return_code"] != 0:
        return "agent_error"
    if tests["passed"]:
        return "passed"
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
    replicate_id = args.replicate_id or inferred_replicate_id(output)
    if replicate_id is not None and not re.fullmatch(r"[A-Za-z0-9._-]+", replicate_id):
        raise RuntimeError("--replicate-id must contain only letters, digits, '.', '_', or '-'")
    comparison_id = args.comparison_id
    if comparison_id is not None and not re.fullmatch(r"[A-Za-z0-9._-]+", comparison_id):
        raise RuntimeError("--comparison-id must contain only letters, digits, '.', '_', or '-'")
    task_ids = selected_task_ids(args.tasks)
    validate_timeout("--timeout", args.timeout)
    validate_timeout("--test-timeout", args.test_timeout)
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
                    "replicate_id": replicate_id,
                    "comparison_id": comparison_id,
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
        "replicate_id": replicate_id,
        "comparison_id": comparison_id,
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
        trace_file = task_dir / "agent-trace.jsonl"
        command = render_agent_command(args.agent_command, prompt_file, work, trace_file)
        print(f"[{index}/{len(task_ids)}] {task_id}", flush=True)
        agent = run_process(command, work, args.timeout, task_dir / "agent.log")
        partial_trace = Path(str(trace_file) + ".partial")
        agent["trace"] = (str(partial_trace) if partial_trace.is_file() else
                          str(trace_file) if trace_file.is_file() else None)
        agent["telemetry"] = reconcile_process_telemetry(
            load_trace_summary(trace_file),
            timed_out=agent["timed_out"],
            return_code=agent["return_code"],
        )
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
