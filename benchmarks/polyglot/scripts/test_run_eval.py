import importlib.util
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
