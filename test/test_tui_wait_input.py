#!/usr/bin/env python3
"""Real ncurses regression: input/paste during nested HTTP wait frames.

No provider requests or personal config. The fixture replaces agent_entry only
in an isolated test home and coordinates a blocking wait through marker files.
"""
import base64
import fcntl
from http.server import BaseHTTPRequestHandler, HTTPServer
import threading
import json
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else "build/capstan").resolve()
PLUGIN = r'''
local json = require("vendor.rxi.json")
local function mark(name, value)
  local file = assert(io.open(name, "w"))
  file:write(value or "ok")
  file:close()
end
local function wait()
  agent.set_activity("Delegating")
  agent.set_thinking(true)
  mark("waiting")
  for _ = 1, 1000 do
    local release = io.open("release", "r")
    if release then release:close(); break end
    http.wait_frame()
  end
  agent.set_thinking(false)
  agent.set_activity(nil)
  mark("finished")
end
_G.fixture_wait = wait
_G.should_auto_compact = function() return false end
_G.agent_entry = function(messages)
  local input = messages[#messages].content
  if input == "active" then
    http.post_stream("FIXTURE_STREAM_URL", "{}", {}, function(_, done)
      if done then agent.finish_run() end
    end)
    mark("running")
    return -- A local HTTP fixture keeps the stream open until Esc cancellation.
  elseif input == "fold" or input == "fold_wait" then
    agent.append_ui('Shell: fixture\n', 'agent')
    local body = '[exit 0]\n'
    for i = 1, 21 do body = body .. string.format('BODY_%02d\n', i) end
    agent.append_ui(body, 'agent', 'shell')
    if input == 'fold_wait' then wait() end
  elseif input == "permit_handoff" or input == "permit_new" then
    if input == "permit_handoff" then wait() end
    mark("decision", permit.prompt("fixture", "fixture target"))
  elseif input == "delegate" then
    wait()
  else
    mark("captured", json.encode(messages))
  end
  agent.finish_run()
end
return {id = "wait_input_test", command = "/wait", history = false,
  handler = function() wait(); return "wait complete" end}
'''


def sgr_attributes(prefix):
    foreground = None
    bold = dim = False
    for match in re.finditer(rb"\x1b\[([0-9;]*)m", prefix):
        codes = [int(c or b"0") for c in match[1].split(b";")]
        i = 0
        while i < len(codes):
            code = codes[i]
            if code == 0:
                foreground = None
                bold = dim = False
            elif code == 1: bold = True
            elif code == 2: dim = True
            elif code == 22: bold = dim = False
            elif code == 39: foreground = None
            elif 30 <= code <= 37: foreground = code - 30
            elif 90 <= code <= 97: foreground = code - 90 + 8
            elif code == 38 and codes[i + 1:i + 2] == [5]:
                foreground = codes[i + 2]
                i += 2
            i += 1
    return foreground, bold, dim


def run_case(case):
    with tempfile.TemporaryDirectory(prefix="tui-wait-", dir="build") as tmp:
        root = Path(tmp).resolve()
        home = root / "home"
        config = home / ".config/capstan"
        (config / "plugins").mkdir(parents=True)
        (home / ".local/state").mkdir(parents=True)
        (config / "config.lua").write_text('''return {
  provider = "fixture",
  providers = {fixture = {model = "fixture", context_limit = 4096,
    models = {{id = "fixture", context_limit = 4096}}}},
}\n''')
        if case.startswith("file_image"):
            fixture = Path(__file__).resolve().parent / "fixtures/vision-shapes.png"
            (root / "Снимок.png").write_bytes(fixture.read_bytes())
        stream_release = threading.Event()
        server = None
        stream_url = "unused"
        if case == "autocomplete_busy":
            class StreamHandler(BaseHTTPRequestHandler):
                def do_POST(self):
                    self.rfile.read(int(self.headers.get("Content-Length", "0")))
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()
                    self.wfile.flush()
                    stream_release.wait(30)

                def log_message(self, *_):
                    pass

            server = HTTPServer(("127.0.0.1", 0), StreamHandler)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            stream_url = f"http://127.0.0.1:{server.server_port}/stream"
        (config / "plugins/wait_input_test.lua").write_text(
            PLUGIN.replace("FIXTURE_STREAM_URL", stream_url))
        (config / "plugins/wait_pick_test.lua").write_text(r'''
return {id = "wait_pick_test", command = "/waitpick", history = false,
  autocomplete = {title = "Wait selection", fetch = function() return {"chosen"} end},
  handler = function(ctx)
    assert(ctx.input == " /waitpick" and ctx.args[1] == "chosen")
    fixture_wait()
    return "wait complete"
  end}
''')
        master, slave = pty.openpty()
        height = 50 if case.startswith("shell") else 30
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, 110, 0, 0))
        env = {"HOME": str(home), "TERM": "xterm-256color",
               "PATH": os.defpath, "LANG": "en_US.UTF-8",
               "XDG_CONFIG_HOME": str(home / ".config"),
               "XDG_STATE_HOME": str(home / ".local/state")}
        proc = subprocess.Popen([str(BINARY), "--no-mcp", "--no-wiki",
                                 "--workdir", str(root), "--workspace", str(root)],
                                stdin=slave, stdout=slave, stderr=slave,
                                env=env, cwd=root, start_new_session=True)
        os.close(slave)
        screen = bytearray()

        def drain():
            if select.select([master], [], [], 0.02)[0]:
                try:
                    screen.extend(os.read(master, 65536))
                except OSError:
                    pass

        def until(predicate, label):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                drain()
                if predicate():
                    return
                if proc.poll() is not None:
                    raise AssertionError(f"{case}: process exited during {label}")
            # Only fixture-owned logs; never inspect the user's runtime state.
            logs = "\n".join(p.read_text()[-3000:] for p in
                             (home / ".local/state/capstan/logs").rglob("*.jsonl"))
            raise AssertionError(f"{case}: timeout during {label}\n{logs}\n"
                                 f"screen tail: {bytes(screen[-2500:])!r}\n"
                                 f"fixture files: {[str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()]}")

        def send(text):
            os.write(master, text.encode("utf-8"))

        def pause(seconds=0.2):
            # Split a paste across more than the old 100 ms body timeout.
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                drain()

        try:
            until(lambda: b"\x1b[?2004h" in screen, "terminal startup")
            # Wait for initial rendering after plugin and session initialization.
            until(lambda: b"ready" in screen, "initial screen")
            if case.startswith("file_image"):
                send(" /file\t")
                until(lambda: b"Find:" in screen, "file finder")
                send("Снимок")
                pause()
                send("\r")
                until(lambda: b"ctx:file" in screen, "buffered selected image")
                if case == "file_image_clear":
                    send(" /new\r")
                    pause()
                send("inspect selected\r")
                until(lambda: (root / "captured").exists(), "manual image context")
                captured = json.loads((root / "captured").read_text())
                images = [block["image_url"] for message in captured
                          if isinstance(message["content"], list)
                          for block in message["content"] if block["type"] == "image_url"]
                if case == "file_image_clear":
                    assert images == [], "cleared attachment leaked into new session"
                else:
                    expected = base64.b64encode((root / "Снимок.png").read_bytes()).decode()
                    assert images == [{"url": "data:image/png;base64," + expected,
                                       "detail": "auto"}], captured
                    assert expected.encode() not in screen, "image data leaked into terminal"
                print(f"TUI manual image: {case}: ok")
                return

            if case == "autocomplete_busy":
                send("active\r")
                until(lambda: (root / "running").exists(), "active asynchronous run")
                send(" /waitpick\t")
                until(lambda: b"Commands are unavailable" in screen, "blocked Tab")
                assert b"Wait selection" not in screen, "busy autocomplete opened"
                # This timed notification is non-modal: Enter does not dismiss it.
                # Wait for expiry so a new rejection produces a distinct paint.
                pause(1.4)
                before = len(screen)
                send("\r")
                until(lambda: b"Commands are unavailable" in screen[before:], "blocked Enter")
                assert not (root / "waiting").exists(), "busy handler executed"
                send("\x1b")
                until(lambda: b"[stopped]" in screen, "cancel active stream")
                pause(1.4)  # A remaining timed notification blocks new popup windows.
                send("\t")
                until(lambda: b"Wait selection" in screen, "idle autocomplete restored")
                send("\r")
                until(lambda: (root / "waiting").exists(), "idle handler executes with preserved draft")
                (root / "release").touch()
                until(lambda: (root / "finished").exists(), "idle handler finished")
                print(f"TUI command guard: {case}: ok")
                return

            if case.startswith("permit"):
                send(case + "\r")
                prefix = ""
                if case == "permit_handoff":
                    until(lambda: (root / "waiting").exists(), "permission wait")
                    prefix = "draft "
                    send("\x1b[200~" + prefix + "\x1b[20")
                    pause()
                    (root / "release").touch()
                until(lambda: b"Permit: fixture" in screen, "permission dialog")
                if case == "permit_new":
                    send("\x1b[200~")
                else:
                    # Complete a literal, non-end escape across the window handoff.
                    send("x")
                    prefix += "\x1b[20x"
                body = "y\na\tt\r/shell literal\x1b[A"
                send(body + "\x1b[20")
                pause()
                assert not (root / "decision").exists(), "paste decided permission"
                send("1~")
                pause()
                assert not (root / "decision").exists(), "paste end decided permission"
                send("n")
                until(lambda: (root / "decision").exists(), "explicit denial")
                assert (root / "decision").read_text() == "deny"
                send(" suffix\r")
                until(lambda: (root / "captured").exists(), "draft after permission")
                captured = json.loads((root / "captured").read_text())
                assert captured[-1]["content"] == prefix + body.replace("\r", "\n") + " suffix", captured
                print(f"TUI permission paste: {case}: ok")
                return

            if case == "shell_multiline":
                command = "cat <<'END'\n[exit 99]\nEND\nawk 'BEGIN {for (i=1;i<=21;i++) print i}'; false"
                send("\x1b[200~ /shell " + command + "\x1b[201~\r")
                until(lambda: b"ctx:shell" in screen, "multiline shell result")
                before = len(screen)
                send("\r")
                until(lambda: b"22 lines" in screen[before:], "multiline output folded")
                assert b"[exit 1]" in screen[before:], "actual exit status hidden"
                until(lambda: (root / "captured").exists(), "multiline model result")
                captured = json.loads((root / "captured").read_text())
                assert captured[-1]["content"].startswith("[exit 1]\n[exit 99]\n"), captured
                print(f"TUI shell output: {case}: ok")
                return

            if case.startswith("shell"):
                manual = case == "shell_manual"
                if manual:
                    send(" /shell awk 'BEGIN {for (i=1;i<=21;i++) printf \"BODY_%02d\\n\",i}'\r")
                    until(lambda: b"ctx:shell" in screen, "buffered shell result")
                    send("\r")
                else:
                    send("fold_wait\r" if case == "shell_wait" else "fold\r")
                until(lambda: b"[+]" in screen and b"21 lines" in screen,
                      "collapsed shell output")
                assert sgr_attributes(bytes(screen).split(b"[+]", 1)[0]) == (8, True, False), \
                    (case, "expand marker is not bold gray")
                assert sgr_attributes(bytes(screen).split(b"21 lines", 1)[0]) == (8, False, False), \
                    (case, "line count should stay gray, not bold")
                assert b"BODY_01" not in screen, "folded body was painted"
                send("draft")
                until(lambda: b"draft" in screen, "input draft")

                # Fixed fixture layout: first manual message, or user + agent.
                # Status shares the command row; the fold marker is next.
                row = 4 if manual else 7
                def click():
                    if b"\x1b[?1006h" in screen:
                        send(f"\x1b[<0;3;{row}M\x1b[<0;3;{row}m")
                    else:
                        # Older xterm terminfo advertises the X10 protocol.
                        os.write(master, b"\x1b[M" + bytes((32, 35, row + 32)) +
                                 b"\x1b[M" + bytes((35, 35, row + 32)))

                before = len(screen)
                click()
                until(lambda: b"BODY_01" in screen[before:], "expanded shell output")
                # Check the first paint, without waiting for an effect to settle.
                prefix = bytes(screen).split(b"BODY_01", 1)[0]
                assert sgr_attributes(prefix) == (8, False, False), \
                    (case, "shell body is not immediately plain gray")
                minus = bytes(screen).find(b"-", before)
                assert minus >= 0 and sgr_attributes(bytes(screen[:minus])) == (8, True, False), \
                    (case, "collapse marker is not bold gray")
                before = len(screen)
                click()
                until(lambda: b"+" in screen[before:], "collapsed again")
                # A second expansion proves one click toggles exactly once.
                before = len(screen)
                click()
                until(lambda: b"BODY_01" in screen[before:], "second expansion")
                click()
                pause()
                if case == "shell_wait":
                    (root / "release").touch()
                    until(lambda: (root / "finished").exists(), "shell wait end")
                if (root / "captured").exists():
                    captured = json.loads((root / "captured").read_text())
                    if manual:
                        assert "BODY_21" in captured[-1]["content"], "fold changed model data"
                    (root / "captured").unlink()
                send(" suffix\r")
                until(lambda: (root / "captured").exists(), "draft after shell clicks")
                captured = json.loads((root / "captured").read_text())
                assert captured[-1]["content"] == "draft suffix", captured
                print(f"TUI shell output: {case}: ok")
                return

            if case != "normal":
                if case == "autocomplete":
                    send(" /waitpick\t")
                    until(lambda: b"Wait selection" in screen, "autocomplete popup")
                    send("\r")
                else:
                    # Leading whitespace avoids opening slash-command autocomplete.
                    send(" /wait\r" if case == "manual" else "delegate\r")
                until(lambda: (root / "waiting").exists(), "nested wait")

            expected = "typed голос\nvoice_first end"
            if case in ("manual", "autocomplete"):
                send("manual_draft\r")
                until(lambda: b"manual_draft" in screen, "live manual draft")
                assert not (root / "captured").exists(), "nested dispatch"
                (root / "release").touch()
                until(lambda: (root / "finished").exists(), "manual wait end")
                until(lambda: b"wait complete" in screen, "command result popup")
                send("\r suffix\r")  # Dismiss the no-history command's popup.
                expected = "manual_draft suffix"
            else:
                send("typed \x1b[200~голос\nvoice_first")
                until(lambda: b"voice_first" in screen, "live pasted draft")
                pause()
                assert not (root / "captured").exists(), "paste submitted a line"
                if case == "handoff":
                    (root / "release").touch()
                    until(lambda: (root / "finished").exists(), "wait-to-main handoff")
                send("\x1b[20")
                pause()
                send("1~ end\r")
                if case == "queued":
                    pause()
                    assert not (root / "captured").exists(), "queue ran recursively"
                    (root / "release").touch()

            until(lambda: (root / "captured").exists(), "submitted draft")
            messages = json.loads((root / "captured").read_text())
            users = [m["content"] for m in messages if m["role"] == "user"]
            expected_users = (["delegate"] if case in ("queued", "handoff") else [])
            assert users == expected_users + [expected], (case, users)
            print(f"TUI wait input: {case}: ok")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)
            if server is not None:
                stream_release.set()
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    for scenario in ("queued", "handoff", "manual", "normal", "autocomplete", "autocomplete_busy",
                     "permit_handoff", "permit_new", "shell_multiline",
                     "shell", "shell_wait", "shell_manual", "file_image", "file_image_clear"):
        run_case(scenario)
