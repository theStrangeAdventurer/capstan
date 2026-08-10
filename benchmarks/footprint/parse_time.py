#!/usr/bin/env python3
"""Extract portable CPU and peak-RSS fields from /usr/bin/time output."""

from __future__ import annotations

import re
import sys
from pathlib import Path

MACOS_TIMES = re.compile(
    r"(?P<real>[0-9]+(?:\.[0-9]+)?)\s+real\s+"
    r"(?P<user>[0-9]+(?:\.[0-9]+)?)\s+user\s+"
    r"(?P<system>[0-9]+(?:\.[0-9]+)?)\s+sys"
)
MACOS_RSS = re.compile(r"^\s*(?P<rss>[0-9]+)\s+maximum resident set size\s*$", re.MULTILINE)
LINUX_USER = re.compile(r"^\s*User time \(seconds\):\s*(?P<value>[0-9]+(?:\.[0-9]+)?)\s*$", re.MULTILINE)
LINUX_SYSTEM = re.compile(r"^\s*System time \(seconds\):\s*(?P<value>[0-9]+(?:\.[0-9]+)?)\s*$", re.MULTILINE)
LINUX_RSS = re.compile(r"^\s*Maximum resident set size \(kbytes\):\s*(?P<rss>[0-9]+)\s*$", re.MULTILINE)


def parse_time_output(text: str) -> tuple[str, str, str, str]:
    """Return user seconds, system seconds, CPU seconds, and RSS bytes."""
    macos_times = MACOS_TIMES.search(text)
    macos_rss = MACOS_RSS.search(text)
    if macos_times and macos_rss:
        user = float(macos_times.group("user"))
        system = float(macos_times.group("system"))
        return (
            macos_times.group("user"),
            macos_times.group("system"),
            f"{user + system:.6f}",
            macos_rss.group("rss"),
        )

    linux_user = LINUX_USER.search(text)
    linux_system = LINUX_SYSTEM.search(text)
    linux_rss = LINUX_RSS.search(text)
    if linux_user and linux_system and linux_rss:
        user = float(linux_user.group("value"))
        system = float(linux_system.group("value"))
        return (
            linux_user.group("value"),
            linux_system.group("value"),
            f"{user + system:.6f}",
            str(int(linux_rss.group("rss")) * 1024),
        )

    return "", "", "", ""


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} TIME_LOG", file=sys.stderr)
        return 2
    print("|".join(parse_time_output(Path(sys.argv[1]).read_text(errors="replace"))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
