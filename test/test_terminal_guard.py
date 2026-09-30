#!/usr/bin/env python3
"""Real TUI fatal-exit recovery in a PTY, with isolated config and no requests."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

BINARY = Path(sys.argv[1]).resolve()


def run_case(sig):
    with tempfile.TemporaryDirectory(prefix="capstan-terminal-") as tmp:
        root = Path(tmp)
        config = root / "home/.config/capstan"
        config.mkdir(parents=True)
        (config / "config.lua").write_text('return {provider="fixture", '
            'providers={fixture={model="fixture", models={{id="fixture"}}}}}\n')
        env = {"HOME": str(root / "home"), "TERM": "xterm-256color",
               "PATH": os.defpath, "LANG": "en_US.UTF-8",
               "XDG_CONFIG_HOME": str(root / "home/.config"),
               "XDG_STATE_HOME": str(root / "home/.local/state")}
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
        original = termios.tcgetattr(slave)
        proc = subprocess.Popen([str(BINARY), "--no-mcp", "--no-wiki"],
                                cwd=root, env=env, stdin=slave,
                                stdout=slave, stderr=slave)
        output = bytearray()

        def drain():
            if select.select([master], [], [], 0.05)[0]:
                output.extend(os.read(master, 65536))

        try:
            deadline = time.monotonic() + 10
            while b"\x1b[?2004h" not in output:
                assert proc.poll() is None, output.decode(errors="replace")
                assert time.monotonic() < deadline, "TUI did not start"
                drain()
            assert not (termios.tcgetattr(slave)[3] & termios.ECHO)
            before_signal = len(output)
            proc.send_signal(sig)
            while proc.poll() is None:
                assert time.monotonic() < deadline, "TUI did not exit"
                drain()
            while select.select([master], [], [], 0)[0]:
                drain()
            assert proc.returncode == -sig, proc.returncode
            restored = termios.tcgetattr(slave)
            # BSD may set PENDIN when canonical processing is restored.
            restored[3] &= ~getattr(termios, "PENDIN", 0)
            original[3] &= ~getattr(termios, "PENDIN", 0)
            assert restored == original, f"terminal attributes changed: {original!r} -> {restored!r}"
            for sequence in (b"\x1b[?2004l", b"\x1b[?1006l",
                             b"\x1b[?1049l", b"\x1b[?25h"):
                assert sequence in output[before_signal:], f"missing reset: {sequence!r}"
            print(f"PASS terminal recovery: {sig}")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)


for fatal in (signal.SIGABRT, signal.SIGSEGV, signal.SIGBUS,
              signal.SIGILL, signal.SIGFPE):
    run_case(fatal)
