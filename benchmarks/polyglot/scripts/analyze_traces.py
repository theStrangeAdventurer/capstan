#!/usr/bin/env python3
"""Summarize Capstan traces without hiding timeouts or incomplete telemetry."""

import argparse
import json
import math
from pathlib import Path
import re
from statistics import mean
import sys


def inferred_replicate_id(root: Path) -> str | None:
    match = re.search(r"(?:^|[-_])(r[0-9]+)$", root.name, re.IGNORECASE)
    return match.group(1).lower() if match else None


def configuration_id(metadata: dict, expected_tasks: list[str], source: Path) -> tuple:
    suite = metadata.get("suite")
    commit = metadata.get("corpus_commit")
    agent_timeout = metadata.get("agent_timeout")
    test_timeout = metadata.get("test_timeout")
    comparison_id = metadata.get("comparison_id")
    if not isinstance(suite, str) or not suite:
        raise ValueError(f"invalid suite: {source}")
    if not isinstance(commit, str) or not commit:
        raise ValueError(f"invalid corpus commit: {source}")
    for name, value in (("agent_timeout", agent_timeout),
                        ("test_timeout", test_timeout)):
        if (not isinstance(value, (int, float)) or isinstance(value, bool)
                or not math.isfinite(value) or value <= 0):
            raise ValueError(f"invalid {name}: {source}")
    if comparison_id is None:
        comparison_key = ("unlabeled", str(source.parent.resolve()))
    elif isinstance(comparison_id, str) and comparison_id:
        comparison_key = ("labeled", comparison_id)
    else:
        raise ValueError(f"invalid comparison_id: {source}")
    return (suite, commit, tuple(expected_tasks), float(agent_timeout),
            float(test_timeout), comparison_key)


def validate_telemetry(telemetry: dict, source: Path) -> None:
    complete = telemetry.get("complete")
    if not isinstance(complete, bool):
        raise ValueError(f"invalid telemetry completion flag: {source}")
    if not complete:
        status = telemetry.get("status")
        if status not in {"partial", "corrupt", "inconsistent"}:
            raise ValueError(f"invalid incomplete telemetry status: {source}")
        expected_partial = status == "partial"
        if telemetry.get("partial") is not expected_partial:
            raise ValueError(f"invalid telemetry partial flag: {source}")
        return
    if telemetry.get("status") != "complete" or telemetry.get("partial") is not False:
        raise ValueError(f"invalid complete telemetry status: {source}")
    terminal = telemetry.get("terminal")
    if not isinstance(terminal, dict):
        raise ValueError(f"missing terminal telemetry: {source}")
    if "ok" in terminal and not isinstance(terminal["ok"], bool):
        raise ValueError(f"invalid terminal ok flag: {source}")
    if "intended_exit_code" in terminal:
        value = terminal["intended_exit_code"]
        if not isinstance(value, int) or isinstance(value, bool):
            raise ValueError(f"invalid intended exit code: {source}")
    for name in ("breakdown", "timing"):
        values = terminal.get(name)
        if values is None:
            continue
        if not isinstance(values, dict):
            raise ValueError(f"invalid telemetry {name}: {source}")
        for key, value in values.items():
            if (not isinstance(value, (int, float)) or isinstance(value, bool)
                    or not math.isfinite(value)
                    or (key != "conservation_error_ms" and value < 0)):
                raise ValueError(f"invalid telemetry {name}.{key}: {source}")
    counts = terminal.get("counts")
    if counts is not None:
        if not isinstance(counts, dict):
            raise ValueError(f"invalid telemetry counts: {source}")
        for key, value in counts.items():
            if (not isinstance(value, int) or isinstance(value, bool)
                    or value < 0):
                raise ValueError(f"invalid telemetry counts.{key}: {source}")


def reconcile_process_telemetry(agent: dict) -> None:
    telemetry = agent.get("telemetry")
    if not isinstance(telemetry, dict) or telemetry.get("complete") is not True:
        return
    terminal = telemetry.get("terminal")
    timed_out = agent.get("timed_out") is True
    return_code = agent.get("return_code")
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
        return

    inconsistent = dict(telemetry)
    inconsistent.update({
        "status": "inconsistent",
        "complete": False,
        "partial": False,
        "error": reason,
    })
    agent["telemetry"] = inconsistent


def validate_result_row(row: dict, source: Path) -> None:
    statuses = {"passed", "agent_timeout", "agent_error", "harness_error",
                "test_failure"}
    if row.get("status") not in statuses:
        raise ValueError(f"invalid result status: {source}")
    total = row.get("seconds")
    if (not isinstance(total, (int, float)) or isinstance(total, bool)
            or not math.isfinite(total) or total < 0):
        raise ValueError(f"invalid result duration: {source}")
    agent = row.get("agent")
    if not isinstance(agent, dict):
        raise ValueError(f"invalid agent result: {source}")
    if not isinstance(agent.get("timed_out"), bool):
        raise ValueError(f"invalid agent timeout flag: {source}")
    return_code = agent.get("return_code")
    if not isinstance(return_code, int) or isinstance(return_code, bool):
        raise ValueError(f"invalid agent return code: {source}")
    if agent_seconds(row) is None:
        raise ValueError(f"invalid agent duration: {source}")
    status = row["status"]
    timed_out = agent["timed_out"]
    if timed_out != (status == "agent_timeout"):
        raise ValueError(f"inconsistent agent timeout status: {source}")
    if not timed_out and ((return_code != 0) != (status == "agent_error")):
        raise ValueError(f"inconsistent agent return code status: {source}")
    telemetry = agent.get("telemetry")
    if telemetry is not None:
        if not isinstance(telemetry, dict):
            raise ValueError(f"invalid agent telemetry: {source}")
        validate_telemetry(telemetry, source)
        reconcile_process_telemetry(agent)


def load_runs(paths: list[Path]) -> dict[str, list[dict]]:
    tasks: dict[str, list[dict]] = {}
    seen_replicates: set[tuple] = set()
    for root in paths:
        metadata_path = root / "metadata.json"
        results_path = root / "results.json"
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if not isinstance(metadata, dict):
            raise ValueError(f"invalid metadata payload: {metadata_path}")
        expected_tasks = metadata.get("tasks")
        if (not isinstance(expected_tasks, list) or not expected_tasks
                or any(not isinstance(task, str) or not task for task in expected_tasks)
                or len(set(expected_tasks)) != len(expected_tasks)):
            raise ValueError(f"invalid task list: {metadata_path}")
        config_id = configuration_id(metadata, expected_tasks, metadata_path)

        configured_id = metadata.get("replicate_id")
        if configured_id is None:
            replicate_id = inferred_replicate_id(root) or f"source:{root.resolve()}"
        elif (isinstance(configured_id, (str, int))
              and not isinstance(configured_id, bool)
              and str(configured_id)):
            replicate_id = str(configured_id)
        else:
            raise ValueError(f"invalid replicate_id: {metadata_path}")
        replicate_key = (config_id, replicate_id)
        if replicate_key in seen_replicates:
            raise ValueError(f"duplicate replicate_id {replicate_id!r}: {root}")
        seen_replicates.add(replicate_key)

        payload = json.loads(results_path.read_text(encoding="utf-8"))
        if isinstance(payload, list):
            rows = payload
        elif isinstance(payload, dict) and isinstance(payload.get("results"), list):
            rows = payload["results"]
        else:
            raise ValueError(f"invalid results payload: {results_path}")

        by_task: dict[str, dict] = {}
        for row in rows:
            if not isinstance(row, dict):
                raise ValueError(f"invalid result row: {results_path}")
            task_id = row.get("task_id")
            legacy_task = row.get("task")
            if (task_id is not None and legacy_task is not None
                    and task_id != legacy_task):
                raise ValueError(f"conflicting result task fields: {results_path}")
            task = task_id if task_id is not None else legacy_task
            if not isinstance(task, str) or task not in expected_tasks:
                raise ValueError(f"invalid result task: {results_path}")
            if task in by_task:
                raise ValueError(f"duplicate result task {task!r}: {results_path}")
            validate_result_row(row, results_path)
            by_task[task] = row

        for task in expected_tasks:
            annotated = dict(by_task.get(task, {"task": task, "_missing": True}))
            annotated["_replicate_id"] = replicate_id
            annotated["_configuration_id"] = config_id
            annotated["_source"] = str(root)
            tasks.setdefault(task, []).append(annotated)
    return tasks


def require_single_configuration(tasks: dict[str, list[dict]],
                                 label: str) -> tuple | None:
    configurations = {
        row.get("_configuration_id")
        for rows in tasks.values()
        for row in rows
    }
    if len(configurations) > 1:
        raise ValueError(f"{label} inputs contain multiple configurations")
    return next(iter(configurations), None)


def agent_seconds(row: dict) -> float | None:
    agent = row.get("agent", {})
    if not isinstance(agent, dict):
        return None
    for key in ("seconds", "duration_seconds", "wall_seconds"):
        value = agent.get(key)
        if (isinstance(value, (int, float)) and not isinstance(value, bool)
                and math.isfinite(value) and value >= 0):
            return float(value)
    return None


def average_seconds(rows: list[dict]) -> float | None:
    values = [value for row in rows if (value := agent_seconds(row)) is not None]
    return mean(values) if values else None


def terminal_telemetry(row: dict) -> dict | None:
    agent = row.get("agent", {})
    telemetry = agent.get("telemetry") if isinstance(agent, dict) else None
    if not isinstance(telemetry, dict) or telemetry.get("complete") is not True:
        return None
    terminal = telemetry.get("terminal")
    return terminal if isinstance(terminal, dict) else None


def complete_telemetry_rows(rows: list[dict]) -> list[dict]:
    return [row for row in rows if terminal_telemetry(row) is not None]


DISPLAY_METRICS = (
    ("breakdown", "model_ms"),
    ("breakdown", "tool_ms"),
    ("breakdown", "permission_wait_ms"),
    ("breakdown", "subagent_wait_ms"),
    ("breakdown", "unattributed_ms"),
    ("counts", "model_requests"),
    ("counts", "tool_calls"),
)


def metric_telemetry_rows(rows: list[dict]) -> list[dict]:
    selected = []
    for row in rows:
        terminal = terminal_telemetry(row)
        if terminal is None:
            continue
        valid = True
        for path in DISPLAY_METRICS:
            value = terminal
            for key in path:
                value = value.get(key) if isinstance(value, dict) else None
            if path[0] == "counts":
                valid_value = (isinstance(value, int) and
                               not isinstance(value, bool) and value >= 0)
            else:
                valid_value = (isinstance(value, (int, float)) and
                               not isinstance(value, bool) and
                               math.isfinite(value) and value >= 0)
            if not valid_value:
                valid = False
                break
        if valid:
            selected.append(row)
    return selected


def avg_telemetry(rows: list[dict], path: tuple[str, ...]) -> float | None:
    values = []
    for row in rows:
        value = terminal_telemetry(row)
        if value is None:
            continue
        for key in path:
            value = value.get(key) if isinstance(value, dict) else None
        if (isinstance(value, (int, float)) and not isinstance(value, bool)
                and math.isfinite(value) and value >= 0):
            values.append(float(value))
    return mean(values) if values else None


def paired_delta(capstan_rows: list[dict], other_rows: list[dict]) -> tuple[float | None, int]:
    def key(row: dict) -> tuple:
        return (row.get("_configuration_id"), row.get("_replicate_id"))

    capstan = {key(row): agent_seconds(row) for row in capstan_rows}
    other = {key(row): agent_seconds(row) for row in other_rows}
    deltas = [capstan[pair] - other[pair]
              for pair in capstan.keys() & other.keys()
              if capstan[pair] is not None and other[pair] is not None]
    return (mean(deltas), len(deltas)) if deltas else (None, 0)


def timeout_count(rows: list[dict]) -> int:
    return sum(isinstance(row.get("agent"), dict)
               and row["agent"].get("timed_out") is True for row in rows)


def agent_error_count(rows: list[dict]) -> int:
    return sum(row.get("status") == "agent_error" for row in rows)


def missing_count(rows: list[dict]) -> int:
    return sum(row.get("_missing") is True for row in rows)


def inconsistent_telemetry_count(rows: list[dict]) -> int:
    return sum(
        isinstance(row.get("agent"), dict)
        and isinstance(row["agent"].get("telemetry"), dict)
        and row["agent"]["telemetry"].get("status") == "inconsistent"
        for row in rows
    )


def metric(value: float | None, scale: float = 1.0, digits: int = 2) -> str:
    return "N/A" if value is None else f"{value / scale:.{digits}f}"


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capstan", type=Path, action="append", required=True)
    parser.add_argument("--other", type=Path, action="append")
    parser.add_argument("--limit", type=positive_int, default=12)
    args = parser.parse_args()
    capstan = load_runs(args.capstan)
    other = load_runs(args.other or [])
    capstan_configuration = require_single_configuration(capstan, "capstan")
    other_configuration = require_single_configuration(other, "other")
    if (other_configuration is not None
            and capstan_configuration != other_configuration):
        raise ValueError("capstan and other inputs use different configurations")
    rows = []
    for task, runs in capstan.items():
        other_runs = other.get(task, [])
        capstan_wall = average_seconds(runs)
        other_wall = average_seconds(other_runs)
        delta, pairs = paired_delta(runs, other_runs)
        trace_rows = complete_telemetry_rows(runs)
        metric_rows = metric_telemetry_rows(runs)
        rows.append((delta, task, capstan_wall, other_wall, runs, other_runs,
                     trace_rows, metric_rows, pairs))
    rows.sort(key=lambda row: (row[0] is not None, row[0] or 0), reverse=True)
    print("task\tcapstan_s\tother_s\tpaired_delta_s\tpairs\tcapstan_timeouts\t"
          "capstan_agent_errors\tcapstan_missing\tother_timeouts\t"
          "other_agent_errors\tother_missing\ttraces\tmetrics\t"
          "inconsistent_telemetry\tmodel_s\ttools_s\tpermissions_s\t"
          "subagents_s\tunattributed_s\trequests\t"
          "tool_calls")
    for (delta, task, wall, other_wall, all_runs, other_runs, trace_runs,
         metric_runs, pairs) in rows[: args.limit]:
        model = avg_telemetry(metric_runs, ("breakdown", "model_ms"))
        tools = avg_telemetry(metric_runs, ("breakdown", "tool_ms"))
        permissions = avg_telemetry(metric_runs,
                                    ("breakdown", "permission_wait_ms"))
        subagents = avg_telemetry(metric_runs,
                                  ("breakdown", "subagent_wait_ms"))
        unknown = avg_telemetry(metric_runs,
                                ("breakdown", "unattributed_ms"))
        requests = avg_telemetry(metric_runs,
                                 ("counts", "model_requests"))
        calls = avg_telemetry(metric_runs, ("counts", "tool_calls"))
        delta_text = "N/A" if delta is None else f"{delta:+.2f}"
        trace_coverage = f"{len(trace_runs)}/{len(all_runs)}"
        metric_coverage = f"{len(metric_runs)}/{len(all_runs)}"
        inconsistent = (
            f"{inconsistent_telemetry_count(all_runs)}/{len(all_runs)}")
        timeouts = f"{timeout_count(all_runs)}/{len(all_runs)}"
        agent_errors = f"{agent_error_count(all_runs)}/{len(all_runs)}"
        missing = f"{missing_count(all_runs)}/{len(all_runs)}"
        other_timeouts = f"{timeout_count(other_runs)}/{len(other_runs)}"
        other_agent_errors = (
            f"{agent_error_count(other_runs)}/{len(other_runs)}")
        other_missing = f"{missing_count(other_runs)}/{len(other_runs)}"
        print(f"{task}\t{metric(wall)}\t{metric(other_wall)}\t{delta_text}\t{pairs}\t"
              f"{timeouts}\t{agent_errors}\t{missing}\t{other_timeouts}\t"
              f"{other_agent_errors}\t{other_missing}\t{trace_coverage}\t"
              f"{metric_coverage}\t{inconsistent}\t"
              f"{metric(model, 1000)}\t{metric(tools, 1000)}\t"
              f"{metric(permissions, 1000)}\t"
              f"{metric(subagents, 1000)}\t{metric(unknown, 1000)}\t"
              f"{metric(requests, digits=1)}\t{metric(calls, digits=1)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
