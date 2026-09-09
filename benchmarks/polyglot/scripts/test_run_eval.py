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


class ImportTests(unittest.TestCase):
    def test_file_loader_without_repository_on_sys_path(self):
        import subprocess

        code = """
import importlib.util
import sys
from pathlib import Path
before = list(sys.path)
assert importlib.util.find_spec('benchmarks') is None
assert importlib.util.find_spec('canonical_logs') is None
spec = importlib.util.spec_from_file_location('trace_consumer', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert sys.path == before
assert module.load_log_summary(Path('absent.jsonl')) is None
"""
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, '-I', '-c', code, str(SCRIPT.resolve())],
                cwd=directory, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


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

    def test_trace_placeholder_requires_explicit_path(self):
        with self.assertRaises(RuntimeError):
            RUN_EVAL.render_agent_command(
                "agent {prompt_file} {workdir} {trace_file}", Path("prompt"), Path("work"))

    def test_common_span_context_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            for span in ("b" * 16, "0" * 16, "B" * 16, "short", None):
                context = {"run_id": "native", "trace_id": "a" * 32, "span_id": span}
                self.write_events(path, [self.event(1, "run.started"),
                                        self.event(2, "run.context", context),
                                        self.event(3, "run.finished", {})])
                summary = RUN_EVAL.load_trace_summary(path)
                if span == "b" * 16:
                    self.assertEqual(summary["correlation"]["span_id"], span)
                else:
                    self.assertEqual(summary["status"], "corrupt")

    def test_common_context_is_additive_and_survives_partial_attempt(self):
        context = {"run_id": "native-run", "trace_id": "a" * 32,
                   "session_id": "saved-session"}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            events = [self.event(1, "run.started"),
                      self.event(2, "run.context", context),
                      self.event(3, "run.finished", {"ok": True})]
            self.write_events(path, events)
            summary = RUN_EVAL.load_trace_summary(path)
            self.assertEqual(summary["status"], "complete")
            self.assertEqual(summary["correlation"],
                             {"legacy_run_id": "run-1", **context})
            path.rename(Path(str(path) + ".partial"))
            partial = RUN_EVAL.load_trace_summary(path)
            self.assertEqual(partial["status"], "partial")
            self.assertEqual(partial["correlation"], summary["correlation"])
            self.assertNotIn("terminal", partial)

    def test_run_start_context_precedes_first_request(self):
        context = {"run_id": "native", "trace_id": "b" * 32}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            for late in (False, True):
                middle = [("run.context", context),
                          ("model.request.started", {"turn": 1, "attempt": 1})]
                if late:
                    middle.reverse()
                events = [self.event(1, "run.started")]
                events += [self.event(i + 2, name, data)
                           for i, (name, data) in enumerate(middle)]
                events += [self.event(4, "model.request.finished", {"turn": 1, "attempt": 1}),
                           self.event(5, "run.finished", {"ok": True})]
                self.write_events(path, events)
                self.assertEqual(RUN_EVAL.load_trace_summary(path)["status"],
                                 "corrupt" if late else "complete")

    def test_invalid_or_duplicate_common_context_is_rejected(self):
        valid = {"run_id": "native", "trace_id": "b" * 32}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            for contexts in ([{**valid, "trace_id": "0" * 32}],
                             [{**valid, "session_id": ""}], [valid, valid]):
                events = [self.event(1, "run.started")]
                events += [self.event(i + 2, "run.context", context)
                           for i, context in enumerate(contexts)]
                events.append(self.event(len(events) + 1, "run.finished", {"ok": True}))
                self.write_events(path, events)
                self.assertEqual(RUN_EVAL.load_trace_summary(path)["status"], "corrupt")

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


class TelemetryEnvironmentTests(unittest.TestCase):
    def test_preserves_resources_and_replaces_stale_identity(self):
        result = RUN_EVAL.telemetry_environment(
            {"attempt_id": "a" * 32, "task": "python/pov", "comparison_id": None,
             "untrusted": "not exported"},
            "service.name=capstan,benchmark.attempt_id=old,benchmark.task=old")
        self.assertEqual(result, {"OTEL_RESOURCE_ATTRIBUTES":
            "service.name=capstan,benchmark.attempt_id=" + "a" * 32 +
            ",benchmark.task=python/pov"})

    def test_rejects_attribute_injection_and_oversize(self):
        for value in ("x,service.name=other", "x=y", "x\\ny", "a" * 129, 1, ""):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                RUN_EVAL.telemetry_environment({"task": value})

    def test_child_receives_context_without_mutating_parent(self):
        original = os.environ.get("OTEL_RESOURCE_ATTRIBUTES")
        command = ("import os; "
                   "assert 'benchmark.attempt_id=" + "a" * 32 +
                   "' in os.environ['OTEL_RESOURCE_ATTRIBUTES']")
        with tempfile.TemporaryDirectory() as directory:
            result = RUN_EVAL.run_process(
                [sys.executable, "-c", command], Path(directory), 5,
                Path(directory) / "agent.log", telemetry_context={"attempt_id": "a" * 32})
        self.assertEqual(result["return_code"], 0)
        self.assertEqual(os.environ.get("OTEL_RESOURCE_ATTRIBUTES"), original)


class AttemptIdentityTests(unittest.TestCase):
    def test_explicit_session_placeholder_and_no_injection(self):
        identity = "a" * 32
        base = "capstan run --benchmark --prompt-file {prompt_file} --workdir {workdir}"
        paths = (Path("prompt with spaces"), Path("work dir"), Path("trace"))
        plain = RUN_EVAL.render_agent_command(base, *paths, identity)
        self.assertNotIn("--session-id", plain)
        self.assertEqual(RUN_EVAL.attempt_identity(base, identity)["identity_transport"],
                         "harness_only")
        template = base + " --session-id {attempt_id}"
        command = RUN_EVAL.render_agent_command(template, *paths, identity)
        self.assertEqual(command, plain + ["--session-id", identity])
        self.assertIn("prompt with spaces", command)
        self.assertEqual(RUN_EVAL.attempt_identity(template, identity)["session_id"], identity)
        for bad in (None, "x; echo injected", "-flag"):
            with self.assertRaises(RuntimeError):
                RUN_EVAL.render_agent_command(template, *paths, bad)


class ArgumentValidationTests(unittest.TestCase):
    def test_canonical_log_rejects_isolation_before_corpus_access(self):
        from argparse import Namespace
        from unittest.mock import patch
        args = Namespace(canonical_log=Path('absent.jsonl'), agent_command=
                         'capstan run --benchmark --session-id {attempt_id}')
        with patch.object(RUN_EVAL, 'parse_args', return_value=args), \
             self.assertRaisesRegex(RuntimeError, 'without --benchmark'):
            RUN_EVAL.main()

    def test_harness_hash_includes_canonical_adapter(self):
        from unittest.mock import patch
        reads = []
        def content(path):
            reads.append(path.name)
            return path.name.encode()
        with patch.object(Path, 'read_bytes', content):
            first = RUN_EVAL.harness_sha256()
        self.assertEqual(reads, ['run_eval.py', 'canonical_logs.py'])
        with patch.object(Path, 'read_bytes', lambda path:
                          b'changed' if path.name == 'canonical_logs.py' else path.name.encode()):
            self.assertNotEqual(first, RUN_EVAL.harness_sha256())

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
