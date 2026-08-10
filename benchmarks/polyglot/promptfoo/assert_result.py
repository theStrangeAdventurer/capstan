"""Score Promptfoo provider output using the canonical upstream-test status."""

from __future__ import annotations

import json


def check_upstream_tests(output: str, context: dict) -> dict:
    try:
        result = json.loads(output)
    except (TypeError, json.JSONDecodeError) as exc:
        return {
            "pass": False,
            "score": 0,
            "reason": f"Provider returned invalid result JSON: {exc}",
            "named_scores": {"upstream_tests": 0, "agent_completed": 0},
        }

    status = result.get("status", "missing_status")
    passed = status == "passed"
    agent = result.get("agent") or {}
    resources = agent.get("resources") or {}
    reason = f"{result.get('task', 'unknown')}: {status}"
    if status == "agent_timeout":
        reason += f" after {agent.get('seconds', '?')}s"
    elif status != "passed":
        metadata = context.get("metadata") or {}
        log_path = metadata.get("agent_log")
        if log_path:
            reason += f"; agent log: {log_path}"
    return {
        "pass": passed,
        "score": 1 if passed else 0,
        "reason": reason,
        "named_scores": {
            "upstream_tests": 1 if passed else 0,
            "agent_completed": 0 if status == "agent_timeout" else 1,
            "agent_wall_seconds": agent.get("seconds", 0),
            "agent_cpu_seconds": resources.get("cpu_seconds", 0),
            "agent_peak_rss_mib": resources.get("max_rss_mib", 0),
        },
    }
