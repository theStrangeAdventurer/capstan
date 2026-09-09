import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock


SCRIPT = Path(__file__).with_name("analyze_traces.py")
SPEC = importlib.util.spec_from_file_location("analyze_traces", SCRIPT)
assert SPEC and SPEC.loader
ANALYZE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZE)


class AnalyzeTraceTests(unittest.TestCase):
    def result(self, wall, model_ms=0, *, complete=True, timed_out=False):
        telemetry = {
            "status": "complete" if complete else "partial",
            "complete": complete,
            "partial": not complete,
        }
        if complete:
            telemetry["terminal"] = {
                "ok": not timed_out,
                "intended_exit_code": -9 if timed_out else 0,
                "breakdown": {
                    "model_ms": model_ms,
                    "tool_ms": 1000,
                    "permission_wait_ms": 200,
                    "subagent_wait_ms": 300,
                    "unattributed_ms": 500,
                },
                "counts": {"model_requests": 2, "tool_calls": 3},
            }
        return {
            "task": "python/pov",
            "status": "agent_timeout" if timed_out else "passed",
            "seconds": wall,
            "agent": {
                "seconds": wall,
                "timed_out": timed_out,
                "return_code": -9 if timed_out else 0,
                "telemetry": telemetry,
            },
        }

    def write_results(self, root: Path, rows, replicate_id="r1", tasks=None,
                      comparison_id="deepseek-v4-medium"):
        expected = tasks or list(dict.fromkeys(row["task"] for row in rows))
        (root / "metadata.json").write_text(
            json.dumps({
                "suite": "mini-v2",
                "corpus_commit": "corpus-1",
                "agent_timeout": 240,
                "test_timeout": 120,
                "tasks": expected,
                "replicate_id": replicate_id,
                "comparison_id": comparison_id,
            }),
            encoding="utf-8",
        )
        (root / "results.json").write_text(
            json.dumps({"results": rows}), encoding="utf-8"
        )

    def test_wall_includes_timeouts_but_breakdown_reports_coverage(self):
        rows = [
            self.result(10.0, 8000, complete=True),
            self.result(240.0, complete=False, timed_out=True),
        ]
        self.assertEqual(ANALYZE.average_seconds(rows), 125.0)
        self.assertEqual(len(ANALYZE.complete_telemetry_rows(rows)), 1)
        self.assertEqual(ANALYZE.timeout_count(rows), 1)

    def test_reports_paired_delta_and_explicit_coverage(self):
        with tempfile.TemporaryDirectory() as capstan_dir, tempfile.TemporaryDirectory() as other_dir:
            capstan = Path(capstan_dir)
            other = Path(other_dir)
            self.write_results(capstan, [self.result(10.0, 8000)])
            self.write_results(other, [self.result(6.0, 0)])
            output = io.StringIO()
            argv = [str(SCRIPT), "--capstan", str(capstan),
                    "--other", str(other)]
            with mock.patch("sys.argv", argv), redirect_stdout(output):
                self.assertEqual(ANALYZE.main(), 0)
            self.assertIn(
                "python/pov\t10.00\t6.00\t+4.00\t1\t0/1\t0/1\t0/1\t"
                "0/1\t0/1\t0/1\t1/1\t1/1\t0/1\t8.00\t1.00\t0.20\t0.30\t"
                "0.50\t2.0\t3.0",
                output.getvalue(),
            )

    def test_comparator_outcomes_are_reported_separately(self):
        with tempfile.TemporaryDirectory() as capstan_dir, tempfile.TemporaryDirectory() as other_dir:
            capstan = Path(capstan_dir)
            other = Path(other_dir)
            self.write_results(capstan, [self.result(10.0)])
            self.write_results(
                other, [self.result(240.0, complete=False, timed_out=True)])
            output = io.StringIO()
            argv = [str(SCRIPT), "--capstan", str(capstan),
                    "--other", str(other)]
            with mock.patch("sys.argv", argv), redirect_stdout(output):
                self.assertEqual(ANALYZE.main(), 0)
            header, row = output.getvalue().splitlines()
            self.assertIn("other_timeouts", header)
            self.assertEqual(row.split("\t")[8], "1/1")

    def test_agent_errors_are_reported_separately(self):
        row = self.result(4.0, complete=False)
        row["status"] = "agent_error"
        row["agent"]["return_code"] = 1
        self.assertEqual(ANALYZE.agent_error_count([row]), 1)
        self.assertEqual(ANALYZE.timeout_count([row]), 0)

    def test_unpaired_runs_do_not_create_comparative_delta(self):
        capstan = [self.result(10.0)]
        other = [self.result(6.0)]
        capstan[0]["_replicate_id"] = 1
        other[0]["_replicate_id"] = 2
        delta, pairs = ANALYZE.paired_delta(capstan, other)
        self.assertIsNone(delta)
        self.assertEqual(pairs, 0)

    def test_load_runs_uses_metadata_ids_independent_of_argument_order(self):
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            first = Path(first_dir)
            second = Path(second_dir)
            self.write_results(first, [self.result(10.0)], replicate_id="r2")
            self.write_results(second, [self.result(20.0)], replicate_id="r1")
            loaded = ANALYZE.load_runs([first, second])["python/pov"]
            self.assertEqual(
                {row["_replicate_id"]: row["agent"]["seconds"] for row in loaded},
                {"r1": 20.0, "r2": 10.0},
            )

    def test_missing_results_count_against_expected_replicates(self):
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            first = Path(first_dir)
            second = Path(second_dir)
            self.write_results(first, [self.result(10.0)], replicate_id="r1")
            self.write_results(second, [], replicate_id="r2", tasks=["python/pov"])
            loaded = ANALYZE.load_runs([first, second])["python/pov"]
            self.assertEqual(len(loaded), 2)
            self.assertEqual(ANALYZE.missing_count(loaded), 1)
            self.assertEqual(len(ANALYZE.complete_telemetry_rows(loaded)), 1)

    def test_rejects_malformed_results_and_duplicate_replicates(self):
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            first = Path(first_dir)
            second = Path(second_dir)
            self.write_results(first, [self.result(10.0)], replicate_id="r1")
            self.write_results(second, [self.result(20.0)], replicate_id="r1")
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([first, second])
            (first / "results.json").write_text("null", encoding="utf-8")
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([first])

    def test_rejects_zero_timeouts_and_conflicting_task_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.result(10.0)
            self.write_results(root, [row])
            metadata_path = root / "metadata.json"
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            metadata["agent_timeout"] = 0
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

            metadata["agent_timeout"] = 240
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            conflicting = self.result(10.0)
            conflicting["task_id"] = "java/series"
            (root / "results.json").write_text(
                json.dumps({"results": [conflicting]}), encoding="utf-8")
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

    def test_report_rejects_mixed_configurations_on_one_side(self):
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            first = Path(first_dir)
            second = Path(second_dir)
            self.write_results(
                first, [self.result(10.0)], replicate_id="r1",
                comparison_id="model-a")
            self.write_results(
                second, [self.result(100.0)], replicate_id="r2",
                comparison_id="model-b")
            argv = [str(SCRIPT), "--capstan", str(first),
                    "--capstan", str(second)]
            with mock.patch("sys.argv", argv), self.assertRaises(ValueError):
                ANALYZE.main()

    def test_report_rejects_mismatched_side_configurations(self):
        with tempfile.TemporaryDirectory() as capstan_dir, tempfile.TemporaryDirectory() as other_dir:
            capstan = Path(capstan_dir)
            other = Path(other_dir)
            self.write_results(
                capstan, [self.result(10.0)], comparison_id="model-a")
            self.write_results(
                other, [self.result(6.0)], comparison_id="model-b")
            argv = [str(SCRIPT), "--capstan", str(capstan),
                    "--other", str(other)]
            with mock.patch("sys.argv", argv), self.assertRaises(ValueError):
                ANALYZE.main()

    def test_configuration_mismatch_is_not_paired(self):
        capstan = [self.result(10.0)]
        other = [self.result(6.0)]
        capstan[0]["_replicate_id"] = "r1"
        other[0]["_replicate_id"] = "r1"
        capstan[0]["_configuration_id"] = ("mini-v2", "a", 240, 120)
        other[0]["_configuration_id"] = ("mini-v2", "b", 240, 120)
        delta, pairs = ANALYZE.paired_delta(capstan, other)
        self.assertIsNone(delta)
        self.assertEqual(pairs, 0)

    def test_comparison_id_and_task_matrix_are_required_for_pairing(self):
        with tempfile.TemporaryDirectory() as capstan_dir, tempfile.TemporaryDirectory() as other_dir:
            capstan = Path(capstan_dir)
            other = Path(other_dir)
            self.write_results(
                capstan, [self.result(10.0)], comparison_id="model-a")
            self.write_results(
                other, [self.result(6.0)], comparison_id="model-b")
            capstan_rows = ANALYZE.load_runs([capstan])["python/pov"]
            other_rows = ANALYZE.load_runs([other])["python/pov"]
            delta, pairs = ANALYZE.paired_delta(capstan_rows, other_rows)
            self.assertIsNone(delta)
            self.assertEqual(pairs, 0)

    def test_rejects_invalid_complete_telemetry_metrics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.result(10.0)
            row["agent"]["telemetry"]["terminal"]["breakdown"]["model_ms"] = -1
            self.write_results(root, [row])
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

            row = self.result(10.0)
            row["agent"]["telemetry"]["terminal"]["counts"]["tool_calls"] = 1.5
            self.write_results(root, [row])
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

    def test_excludes_process_inconsistent_terminal_telemetry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.result(10.0)
            row["status"] = "agent_error"
            row["agent"]["return_code"] = 1
            self.write_results(root, [row])

            loaded = ANALYZE.load_runs([root])["python/pov"]
            self.assertEqual(ANALYZE.average_seconds(loaded), 10.0)
            self.assertEqual(
                loaded[0]["agent"]["telemetry"]["status"], "inconsistent")
            self.assertEqual(ANALYZE.inconsistent_telemetry_count(loaded), 1)
            self.assertEqual(ANALYZE.complete_telemetry_rows(loaded), [])
            self.assertEqual(ANALYZE.metric_telemetry_rows(loaded), [])

            output = io.StringIO()
            argv = [str(SCRIPT), "--capstan", str(root)]
            with mock.patch("sys.argv", argv), redirect_stdout(output):
                self.assertEqual(ANALYZE.main(), 0)
            header, rendered = output.getvalue().splitlines()
            fields = header.split("\t")
            values = rendered.split("\t")
            self.assertEqual(
                values[fields.index("inconsistent_telemetry")], "1/1")

    def test_rejects_inconsistent_agent_status_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            timeout = self.result(10.0, complete=False, timed_out=True)
            timeout["status"] = "passed"
            self.write_results(root, [timeout])
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

            nonzero = self.result(10.0, complete=False)
            nonzero["agent"]["return_code"] = 1
            self.write_results(root, [nonzero])
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

            false_error = self.result(10.0, complete=False)
            false_error["status"] = "agent_error"
            self.write_results(root, [false_error])
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

    def test_rejects_incomplete_result_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_results(root, [{"task": "python/pov"}])
            with self.assertRaises(ValueError):
                ANALYZE.load_runs([root])

    def test_metric_coverage_requires_every_displayed_metric(self):
        complete = self.result(10.0)
        incomplete = self.result(12.0)
        del incomplete["agent"]["telemetry"]["terminal"]["breakdown"]["model_ms"]
        self.assertEqual(len(ANALYZE.complete_telemetry_rows(
            [complete, incomplete])), 2)
        self.assertEqual(len(ANALYZE.metric_telemetry_rows(
            [complete, incomplete])), 1)

    def test_root_metrics_have_independent_coverage_and_missing_is_not_zero(self):
        for explicit in (True, False):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory() as directory:
                measured = self.result(10)
                terminal = measured['agent']['telemetry']['terminal']
                # Root-only canonical measurements do not require legacy breakdowns.
                terminal.pop('counts')
                terminal['breakdown'] = {}
                if explicit:
                    terminal.update(duration_ms=1250, turns=2)
                    terminal['breakdown']['overlap_ms'] = 0
                partial = self.result(240, complete=False, timed_out=True)
                partial['agent']['telemetry']['terminal'] = {
                    'duration_ms': 9999, 'turns': 99, 'breakdown': {'overlap_ms': 99}}
                argv = [str(SCRIPT)]
                for i, row in enumerate((measured, self.result(12), partial)):
                    root = Path(directory) / str(i)
                    root.mkdir()
                    self.write_results(root, [row], replicate_id=f'r{i}')
                    argv.extend(['--capstan', str(root)])
                output = io.StringIO()
                with mock.patch('sys.argv', argv), redirect_stdout(output):
                    self.assertEqual(ANALYZE.main(), 0)
                header, rendered = output.getvalue().splitlines()
                values = dict(zip(header.split('\t'), rendered.split('\t')))
                self.assertEqual(values['metrics'], '1/3')
                for key, expected in [('duration_ms', '1250.00'), ('turns', '2.00'),
                                      ('overlap_ms', '0.00')]:
                    self.assertEqual(values[key], expected if explicit else 'N/A')
                    self.assertEqual(values[key + '_coverage'], '1/3' if explicit else '0/3')

    def test_invalid_root_metrics_are_rejected(self):
        for key, bad_values in [('duration_ms', [-1, True, '2', float('nan'), float('inf')]),
                                ('turns', [-1, True, '2', 1.5]),
                                ('overlap_ms', [-1, True, '2', float('nan')])]:
            for value in bad_values:
                with self.subTest(key=key, value=value):
                    row = self.result(1)
                    terminal = row['agent']['telemetry']['terminal']
                    target = terminal['breakdown'] if key == 'overlap_ms' else terminal
                    target[key] = value
                    with self.assertRaises(ValueError):
                        ANALYZE.validate_result_row(row, Path('fixture'))

    def test_nonfinite_metrics_are_ignored(self):
        self.assertIsNone(ANALYZE.agent_seconds({"agent": {"seconds": float("inf")}}))
        row = self.result(1.0)
        row["agent"]["telemetry"]["terminal"]["breakdown"]["model_ms"] = float("nan")
        self.assertIsNone(ANALYZE.avg_telemetry([row], ("breakdown", "model_ms")))

    def test_boolean_wall_time_is_not_numeric(self):
        self.assertIsNone(ANALYZE.agent_seconds({"agent": {"seconds": True}}))

    def test_backend_links_are_diagnostic_and_optional(self):
        template = ANALYZE.trace_url_template("https://tempo.example/trace/{trace_id}?span={span_id}")
        row = self.result(240, complete=False, timed_out=True)
        row["agent"]["telemetry"]["correlation"] = {
            "run_id": "native", "trace_id": "a" * 32, "span_id": "b" * 16}
        self.assertEqual(ANALYZE.backend_trace_links([row], template)[0]["url"],
                         "https://tempo.example/trace/" + "a" * 32 + "?span=" + "b" * 16)
        self.assertEqual(ANALYZE.metric_telemetry_rows([row]), [])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_results(root, [row])
            output = io.StringIO()
            with mock.patch("sys.argv", [str(SCRIPT), "--capstan", str(root),
                                         "--trace-url-template", template]), redirect_stdout(output):
                self.assertEqual(ANALYZE.main(), 0)
            header, rendered = output.getvalue().splitlines()
            self.assertTrue(header.endswith("backend_traces"))
            self.assertEqual(json.loads(rendered.split("\t")[-1])[0]["status"], "partial")
        del row["agent"]["telemetry"]["correlation"]["span_id"]
        self.assertEqual(ANALYZE.backend_trace_links([row], template), [])
        row["agent"]["telemetry"]["correlation"]["trace_id"] = "0" * 32
        self.assertEqual(ANALYZE.backend_trace_links([row], template), [])
        self.assertEqual(ANALYZE.backend_trace_links([self.result(1)], template), [])

    def test_backend_template_rejects_invalid_links(self):
        for value in ("file:///{trace_id}", "https://user:pass@host/{trace_id}",
                      "https://host/no-id", "https://host/{run_id}/{trace_id}",
                      "https://{trace_id}/", "https://host/{trace_id}\n"):
            with self.subTest(value=value), self.assertRaises(ANALYZE.argparse.ArgumentTypeError):
                ANALYZE.trace_url_template(value)

    def test_limit_must_be_positive(self):
        self.assertEqual(ANALYZE.positive_int("1"), 1)
        for value in ("0", "-1"):
            with self.assertRaises(Exception) as raised:
                ANALYZE.positive_int(value)
            self.assertIsInstance(
                raised.exception, ANALYZE.argparse.ArgumentTypeError)


if __name__ == "__main__":
    unittest.main()
