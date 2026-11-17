#!/usr/bin/env python3
"""Native modal PTY test. No Lua, config, providers, or credential store.
Run: python3 test/test_secret_input.py (builds a dead-stripped native fixture).
"""
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

ROOT = Path(__file__).resolve().parents[1]
FAKE = b"fake-PTY-key-123"
FIXTURE = r'''
#include "tui.h"
#include "input.h"
#include "secret_input.h"
#include <curses.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static void stop(int sig) { g_tui_exit_signal = sig; }
int main(int argc, char **argv) {
  (void)argc;
  assert(tui_secret_prompt("headless", "") == NULL);
  initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
  define_key("\033[200~", TUI_KEY_PASTE_BEGIN);
  signal(SIGTERM, stop); signal(SIGINT, stop);
  input_init(); input_set_text("normal draft remains");
  fputs("\033[?2004h", stdout); fflush(stdout);
  char *key = tui_secret_prompt("Secret fixture", "API key");
  int ok = !strcmp(input_get_text(), "normal draft remains");
  if (!strcmp(argv[1], "accept")) ok &= key && !strcmp(key, "fake-PTY-key-123");
  else ok &= key == NULL;
  if (key) {
    volatile char *p = key;
    for (size_t i = 0, n = strlen(key); i < n; i++) p[i] = 0;
    free(key);
  }
  curs_set(1); endwin();
  fputs("\033[?2004l", stdout); fflush(stdout);
  return ok ? 0 : 2;
}
'''


def run_case(binary, root, mode, payload, resize=False, sig=None, retry=False):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    original = termios.tcgetattr(slave)
    env = {"HOME": str(root), "TERM": "xterm-256color", "PATH": os.defpath}
    proc = subprocess.Popen([str(binary), mode], cwd=root, env=env,
                            stdin=slave, stdout=slave, stderr=slave)
    output = bytearray()

    def drain(duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            if select.select([master], [], [], 0.02)[0]:
                output.extend(os.read(master, 65536))

    try:
        deadline = time.monotonic() + 3
        while b"Secret fixture" not in output:
            assert proc.poll() is None, "fixture exited before modal"
            assert time.monotonic() < deadline, "modal did not appear"
            drain(0.02)
        os.write(master, payload)
        drain(0.2)
        assert proc.poll() is None, "paste or typing submitted without explicit Enter"
        if resize:
            for rows, cols in ((3, 8), (30, 100)):
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                drain(0.15)
        if retry:
            os.write(master, b"\r")  # Invalid paste must be rejected, not submitted.
            drain(0.15)
            assert proc.poll() is None, "invalid paste accepted"
            os.write(master, FAKE)
            drain(0.1)
        if sig:
            proc.send_signal(sig)
        else:
            os.write(master, b"\r" if mode == "accept" else b"\x1b")
        deadline = time.monotonic() + 3
        while proc.poll() is None:
            assert time.monotonic() < deadline, "modal did not exit"
            drain(0.02)
        drain(0.05)
        assert proc.returncode == 0, "wrong native result or modified normal draft"
        assert FAKE not in output and b"bad-secret-line" not in output, "secret echoed"
        restored = termios.tcgetattr(slave)
        for attrs in (restored, original):
            attrs[3] &= ~getattr(termios, "PENDIN", 0)
        assert restored == original, "terminal mode not restored"
        assert b"\x1b[?25h" in output, "cursor not restored"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


with tempfile.TemporaryDirectory(prefix="secret-input-", dir=ROOT / "build") as tmp:
    root = Path(tmp)
    source, binary = root / "fixture.c", root / "fixture"
    source.write_text(FIXTURE)
    nc = ROOT / "vendor/ncurses-install"
    command = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
               "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200112L", "-ffunction-sections", "-fdata-sections",
               "-Iinclude", "-Ivendor/lua-5.5.0/src", f"-I{nc}/include", f"-I{nc}/include/ncursesw",
               "src/tui.c", "src/secret_input.c", "src/input.c", str(source),
               str(nc / "lib/libncursesw.a"), str(nc / "lib/libtinfow.a"),
               "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
               "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True, timeout=30)
    run_case(binary, root, "accept", FAKE + b"X\x7f")
    run_case(binary, root, "accept", b"\x1b[200~" + FAKE + b"\x1b[201~", resize=True)
    run_case(binary, root, "accept", b"\x1b[200~bad-secret-line\r\nother\x1b[201~", retry=True)
    run_case(binary, root, "cancel", FAKE, resize=True)
    run_case(binary, root, "cancel", b"\x1b[200~" + FAKE, sig=signal.SIGTERM)
    print("PASS secret modal: submit, paste, invalid multiline, resize, cancel, signal, draft, no echo")
