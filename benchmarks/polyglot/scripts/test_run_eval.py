import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("run_eval.py")
SPEC = importlib.util.spec_from_file_location("run_eval", SCRIPT)
assert SPEC and SPEC.loader
RUN_EVAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN_EVAL)


class TraceTests(unittest.TestCase):
    def event(self, seq, name, data=None, run_id="run-1", elapsed=None):
        return {
            "schema": "capstan.trace.v1",
            "seq": seq,
            "timestamp_ms": 1000 + seq,
            "elapsed_ms": seq if elapsed is None else elapsed,
            "run_id": run_id,
            "event": name,
            "data": data or {},
        }

    def write_events(self, path, events, trailing_newline=True):
        content = "\n".join(json.dumps(event) for event in events)
        if trailing_newline:
            content += "\n"
        path.write_text(content, encoding="utf-8")

    def test_loads_terminal_trace_summary_without_payload_collision(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            terminal = {
                "ok": True,
                "turns": 2,
                "status": "payload-status",
                "complete": False,
                "counts": {"model_requests": 0, "tool_calls": 0},
            }
            self.write_events(path, [
                self.event(1, "run.started"),
                self.event(2, "run.finished", terminal),
            ])
            summary = RUN_EVAL.load_trace_summary(path)
            self.assertEqual(summary["status"], "complete")
            self.assertTrue(summary["complete"])
            self.assertFalse(summary["partial"])
            self.assertEqual(summary["terminal"], terminal)
            self.assertEqual(summary["observed_events"], 2)

    def test_rejects_structurally_invalid_json_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            for content in ("null\n", "[]\n", '{"data":null}\n'):
                path.write_text(content, encoding="utf-8")
                summary = RUN_EVAL.load_trace_summary(path)
                self.assertEqual(summary["status"], "corrupt")
                self.assertFalse(summary["complete"])

    def test_rejects_invalid_field_types_schema_and_nonfinite_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            invalid_events = (
                {**self.event(1, "run.started"), "run_id": []},
                {**self.event(1, "run.started"), "seq": True},
                {**self.event(1, "run.started"), "schema": "other.trace.v1"},
                {**self.event(1, "run.started"), "elapsed_ms": float("inf")},
            )
            for event in invalid_events:
                self.write_events(path, [event])
                summary = RUN_EVAL.load_trace_summary(path)
                self.assertEqual(summary["status"], "corrupt")

    def test_does_not_trust_terminal_data_from_partial_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            partial = Path(str(path) + ".partial")
            self.write_events(partial, [
                self.event(1, "run.started"),
                self.event(2, "run.finished", {
                    "ok": True,
                    "breakdown": {"model_ms": 99},
                }),
            ])
            summary = RUN_EVAL.load_trace_summary(path)
            self.assertEqual(summary["status"], "partial")
            self.assertTrue(summary["partial"])
            self.assertNotIn("terminal", summary)

    def test_partial_trace_takes_precedence_over_older_published_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            partial = Path(str(path) + ".partial")
            self.write_events(path, [
                self.event(1, "run.started", run_id="old"),
                self.event(2, "run.finished", {"ok": True}, run_id="old"),
            ])
            self.write_events(partial, [
                self.event(1, "run.started", run_id="new"),
            ])
            summary = RUN_EVAL.load_trace_summary(path)
            self.assertEqual(summary["status"], "partial")
            self.assertEqual(summary["observed_events"], 1)
            self.assertEqual(summary["source"], str(partial))

    def test_partial_trace_keeps_valid_prefix_and_marks_truncated_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            partial = Path(str(path) + ".partial")
            valid = json.dumps(self.event(1, "run.started"))
            partial.write_text(valid + "\n{\"broken\"", encoding="utf-8")
            summary = RUN_EVAL.load_trace_summary(path)
            self.assertEqual(summary["status"], "partial")
            self.assertEqual(summary["observed_events"], 1)
            self.assertTrue(summary["truncated_tail"])

    def test_reconciles_terminal_trace_with_process_result(self):
        telemetry = {
            "status": "complete",
            "complete": True,
            "partial": False,
            "terminal": {"ok": True, "intended_exit_code": 0},
        }
        self.assertIs(
            RUN_EVAL.reconcile_process_telemetry(
                telemetry, timed_out=False, return_code=0),
            telemetry,
        )

        timed_out = RUN_EVAL.reconcile_process_telemetry(
            telemetry, timed_out=True, return_code=-9)
        self.assertEqual(timed_out["status"], "inconsistent")
        self.assertFalse(timed_out["complete"])
        self.assertIn("timed out", timed_out["error"])

        failed_telemetry = dict(telemetry)
        failed_telemetry["terminal"] = {
            "ok": False, "intended_exit_code": 1}
        self.assertIs(
            RUN_EVAL.reconcile_process_telemetry(
                failed_telemetry, timed_out=False, return_code=1),
            failed_telemetry,
        )

        wrong_ok = RUN_EVAL.reconcile_process_telemetry(
            telemetry, timed_out=False, return_code=1)
        self.assertEqual(wrong_ok["status"], "inconsistent")
        self.assertIn("terminal ok", wrong_ok["error"])

        wrong_exit = dict(telemetry)
        wrong_exit["terminal"] = {"ok": False, "intended_exit_code": 1}
        reconciled = RUN_EVAL.reconcile_process_telemetry(
            wrong_exit, timed_out=False, return_code=2)
        self.assertEqual(reconciled["status"], "inconsistent")
        self.assertIn("intended_exit_code", reconciled["error"])

    def test_rejects_fractional_terminal_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            self.write_events(path, [
                self.event(1, "run.started"),
                self.event(2, "run.finished", {
                    "counts": {"model_requests": 1.5, "tool_calls": 0},
                }),
            ])
            self.assertEqual(RUN_EVAL.load_trace_summary(path)["status"],
                             "corrupt")

    def test_published_trace_rejects_mismatched_terminal_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            self.write_events(path, [
                self.event(1, "run.started"),
                self.event(2, "run.finished", {
                    "counts": {"model_requests": 1, "tool_calls": 0},
                }),
            ])
            self.assertEqual(RUN_EVAL.load_trace_summary(path)["status"],
                             "corrupt")

    def test_published_trace_requires_paired_model_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            self.write_events(path, [
                self.event(1, "run.started"),
                self.event(2, "model.request.finished", {
                    "turn": 1, "attempt": 1,
                }),
                self.event(3, "run.finished", {
                    "counts": {"model_requests": 1, "tool_calls": 0},
                }),
            ])
            self.assertEqual(RUN_EVAL.load_trace_summary(path)["status"],
                             "corrupt")

    def test_published_trace_requires_matching_tool_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            self.write_events(path, [
                self.event(1, "run.started"),
                self.event(2, "tool.started", {"name": "file_read"}),
                self.event(3, "tool.finished", {"name": "file_write"}),
                self.event(4, "run.finished", {
                    "counts": {"model_requests": 0, "tool_calls": 1},
                }),
            ])
            self.assertEqual(RUN_EVAL.load_trace_summary(path)["status"],
                             "corrupt")


class ArgumentValidationTests(unittest.TestCase):
    def test_task_selection_rejects_duplicates_and_empty_ids(self):
        self.assertEqual(
            RUN_EVAL.selected_task_ids("python/pov, rust/acronym"),
            ("python/pov", "rust/acronym"),
        )
        with self.assertRaises(RuntimeError):
            RUN_EVAL.selected_task_ids("python/pov,python/pov")
        with self.assertRaises(RuntimeError):
            RUN_EVAL.selected_task_ids("python/pov,")

    def test_timeouts_must_be_positive(self):
        self.assertEqual(RUN_EVAL.validate_timeout("--timeout", 1), 1)
        for value in (0, -1):
            with self.assertRaises(RuntimeError):
                RUN_EVAL.validate_timeout("--timeout", value)


class ClassificationTests(unittest.TestCase):
    def test_agent_error_is_not_hidden_by_passing_tests(self):
        agent = {"timed_out": False, "return_code": 1}
        tests = {"passed": True, "steps": []}
        self.assertEqual(RUN_EVAL.classify(agent, tests), "agent_error")


class PrimaryPidRssTests(unittest.TestCase):
    def test_reads_current_process_rss(self):
        rss = RUN_EVAL._pid_rss_bytes(os.getpid())
        self.assertIsNotNone(rss)
        self.assertGreater(rss, 0)

    def test_run_process_excludes_large_child_from_primary_pid_peak(self):
        child = (
            "import time; "
            "payload = bytearray(96 * 1024 * 1024); "
            "time.sleep(0.4)"
        )
        parent = (
            "import subprocess, sys; "
            f"subprocess.run([sys.executable, '-c', {child!r}], check=True)"
        )
        with tempfile.TemporaryDirectory() as directory:
            result = RUN_EVAL.run_process(
                [sys.executable, "-c", parent],
                Path(directory),
                5,
                Path(directory) / "agent.log",
            )

        self.assertEqual(result["return_code"], 0)
        self.assertFalse(result["timed_out"])
        self.assertGreater(result["resources"]["max_rss_bytes"], 0)
        self.assertLess(result["resources"]["max_rss_mib"], 64)

    def test_run_process_replaces_and_cleans_primary_pid_file(self):
        command = (
            "import os, pathlib, sys; "
            "path = pathlib.Path(os.environ['CAPSTAN_BENCH_AGENT_PID_FILE']); "
            "sys.exit(7) if path.exists() else path.write_text(str(os.getpid()))"
        )
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "agent.log"
            pid_path = Path(directory) / "agent-primary.pid"
            pid_path.write_text(str(os.getpid()), encoding="utf-8")
            result = RUN_EVAL.run_process(
                [sys.executable, "-c", command],
                Path(directory),
                5,
                log_path,
            )

            self.assertEqual(result["return_code"], 0)
            self.assertFalse(pid_path.exists())


if __name__ == "__main__":
    unittest.main()
