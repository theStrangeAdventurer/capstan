from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parent


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AssertionTests(unittest.TestCase):
    def setUp(self):
        self.module = load("assert_result", ROOT / "assert_result.py")

    def test_passes_only_canonical_passed_status(self):
        result = self.module.check_upstream_tests(
            json.dumps({"task": "go/transpose", "status": "passed"}),
            {},
        )
        self.assertTrue(result["pass"])
        self.assertEqual(result["named_scores"]["upstream_tests"], 1)

    def test_timeout_is_a_failed_completed_metric(self):
        result = self.module.check_upstream_tests(
            json.dumps(
                {
                    "task": "python/pov",
                    "status": "agent_timeout",
                    "agent": {"seconds": 180.1},
                }
            ),
            {},
        )
        self.assertFalse(result["pass"])
        self.assertEqual(result["named_scores"]["agent_completed"], 0)
        self.assertIn("agent_timeout", result["reason"])

    def test_invalid_json_fails_closed(self):
        result = self.module.check_upstream_tests("not json", {})
        self.assertFalse(result["pass"])
        self.assertIn("invalid result JSON", result["reason"])


class TestGeneratorTests(unittest.TestCase):
    def test_generates_exact_fixed_suite(self):
        module = load("promptfoo_tests", ROOT / "tests.py")
        fake_harness = mock.Mock()
        fake_harness.DEFAULT_TASKS = ("cpp/clock", "go/transpose")
        fake_harness.SUITE = "mini-v2"
        fake_harness.validate_corpus.return_value = "pinned"
        fake_harness.task_source.side_effect = [
            ("cpp", Path("/tmp/clock")),
            ("go", Path("/tmp/transpose")),
        ]
        fake_harness.public_prompt.side_effect = ["clock prompt\n", "transpose prompt\n"]
        with mock.patch.object(module, "_load_harness", return_value=fake_harness):
            cases = module.generate_tests({"corpus": "/tmp/corpus"})
        self.assertEqual([case["vars"]["task"] for case in cases], list(fake_harness.DEFAULT_TASKS))
        self.assertTrue(all(case["metadata"]["corpus_commit"] == "pinned" for case in cases))


class ResourceMeasurementTests(unittest.TestCase):
    def setUp(self):
        self.harness = load("resource_harness", ROOT.parent / "scripts" / "run_eval.py")

    def test_max_rss_normalization_is_positive(self):
        value = self.harness._max_rss_bytes(1024)
        expected = 1024 if sys.platform == "darwin" else 1024 * 1024
        self.assertEqual(value, expected)

    def test_render_agent_command_expands_repo_root(self):
        command = self.harness.render_agent_command(
            "{repo_root}/build/capstan run --prompt-file {prompt_file} --workdir {workdir}",
            Path("/tmp/prompt.md"),
            Path("/tmp/work"),
        )
        self.assertEqual(command[0], str(self.harness.REPO_ROOT / "build" / "capstan"))
        self.assertEqual(command[-4:], ["--prompt-file", "/tmp/prompt.md", "--workdir", "/tmp/work"])

    def test_run_process_records_cpu_and_peak_rss(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.harness.run_process(
                [sys.executable, "-c", "sum(range(100000))"],
                root,
                5,
                root / "agent.log",
            )
        self.assertEqual(result["return_code"], 0)
        self.assertFalse(result["timed_out"])
        self.assertGreater(result["resources"]["max_rss_bytes"], 0)
        self.assertGreaterEqual(result["resources"]["cpu_seconds"], 0)


if __name__ == "__main__":
    unittest.main()
