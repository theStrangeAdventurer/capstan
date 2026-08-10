import importlib.util
import unittest
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "parse_time", Path(__file__).with_name("parse_time.py")
)
parse_time = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(parse_time)


class ParseTimeTests(unittest.TestCase):
    def test_parses_macos_time_l_output(self):
        output = " 30.89 real 0.21 user 0.06 sys\n 16777216 maximum resident set size\n"
        self.assertEqual(
            parse_time.parse_time_output(output),
            ("0.21", "0.06", "0.270000", "16777216"),
        )

    def test_parses_linux_time_v_output(self):
        output = (
            "\tUser time (seconds): 1.25\n"
            "\tSystem time (seconds): 0.75\n"
            "\tMaximum resident set size (kbytes): 4096\n"
        )
        self.assertEqual(
            parse_time.parse_time_output(output),
            ("1.25", "0.75", "2.000000", "4194304"),
        )

    def test_returns_empty_fields_without_time_data(self):
        self.assertEqual(parse_time.parse_time_output("agent stderr\n"), ("", "", "", ""))


if __name__ == "__main__":
    unittest.main()
