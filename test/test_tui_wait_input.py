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
import signal
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
  elseif input == "tasks" or input == "tasks_wait" then
    local tasks = require('agent.tasks')
    local items = {}
    for i = 1, 12 do
      items[i] = {id='t'..i, title=string.format('TASK_%02d', i), status='pending'}
    end
    items[1].title = items[1].title .. string.rep(' long', 30) .. ' WRAPPED_END'
    items[2].title = 'Зафиксировать контракт OpenTelemetry'
    assert(tasks.update({revision=0, tasks=items}))
    if input == 'tasks_wait' then wait() end
  elseif input == "markdown" then
    local body = '**MD_BOLD** *MD_ITALIC* MD_PLAIN\n\n' ..
      '| Key | Value |\n| :--- | ---: |\n| item | **MD_CELL** |\n\n' ..
      '```diff\n- **MD_OLD**\n+ *MD_NEW*\n```\nMD_LAST'
    mark('markdown_source', body)
    agent.append(body, 'agent')
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
        if case == "tasks_collapsed":
            (config / "plugins/tasks_config_test.lua").write_text(
                'capstan.config.tasks = {expanded_by_default=false}\nreturn {id="tasks_config_test"}\n')
        if case.startswith("reasoning"):
            (config / "config.lua").write_text('''return {
  provider = "fixture",
  providers = {fixture = {model = "fixture", context_limit = 4096,
    default_reasoning_efforts = {"high", "low", "medium"},
    models = {{id = "fixture", context_limit = 4096}}}},
}\n''')
            (config / "plugins/reasoning_test.lua").write_text(r'''
local json = require('vendor.rxi.json')
local step = capstan.agent.step_reasoning_effort
local count = 0
capstan.agent.step_reasoning_effort = function(direction)
  local value, err = step(direction)
  count = count + 1
  local file = assert(io.open('effort', 'w'))
  file:write(json.encode({count = count, value = value, error = err,
    info = capstan.models.effective()}))
  file:close()
  return value, err
end
return {id = 'reasoning_test'}
''')
        if case.startswith("file_image"):
            fixture = Path(__file__).resolve().parent / "fixtures/vision-shapes.png"
            (root / "Снимок.png").write_bytes(fixture.read_bytes())
        if case == "workspace_footer":
            git_env = {"PATH": os.defpath, "HOME": str(home),
                       "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null"}
            def git(*args):
                subprocess.run(["git", *args], cwd=root, env=git_env,
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            git("init", "-q")
            (root / ".gitignore").write_text("*\n!tracked\n")
            (root / "tracked").write_text("old\n" * 34)
            git("add", "tracked")
            git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                "-c", "commit.gpgsign=false", "commit", "-qm", "initial")
            (root / "tracked").write_text("new\n" * 128)
        if case == "workspace_footer_custom":
            (config / "plugins/footer_test.lua").write_text(r'''
capstan.config.vcs = {default = "custom", adapters = {
  custom = {commands = {
    status = {"printf", "custom status"},
    summary = {"printf", "%s", "files-v1\nM\t128\t34\tfile\ndone\n"}}},
  unsupported = {commands = {status = {"printf", "old status"}}},
}}
return {id = "footer_test", command = "/footer_switch", history = false,
  handler = function()
    assert(require('agent.vcs').select('unsupported'))
    return 'switched'
  end}
''')
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
        if case == "session_copy":
            commands = root / "bin"
            commands.mkdir()
            for name in ("pbcopy", "wl-copy", "xclip", "xsel"):
                script = commands / name
                script.write_text('#!/bin/sh\n/bin/cat > copied\n')
                script.chmod(0o700)
            env["PATH"] = str(commands) + os.pathsep + os.defpath
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
            if case == "session_copy":
                def active_header():
                    directory = next((home / '.local/state/capstan/sessions').iterdir())
                    identity = (directory / 'active').read_text().strip()
                    return json.loads((directory / (identity + '.jsonl')).read_text().splitlines()[0])
                def copy_row(row, width, expected):
                    width -= 2  # Copy mark is inside the overlay border.
                    pause(0.6)
                    copied = root / 'copied'
                    copied.unlink(missing_ok=True)
                    if b'\x1b[?1006h' in screen:
                        send(f'\x1b[<0;{width - 1};{row}M\x1b[<0;{width - 1};{row}m')
                    else:
                        os.write(master, b'\x1b[M' + bytes((32, width + 31, row + 32)) +
                                 b'\x1b[M' + bytes((35, width + 31, row + 32)))
                    until(lambda: copied.exists() and copied.read_text() == expected, 'full session copy')
                header = active_header()
                assert b'session.id:' not in screen and b'[x]' not in screen, 'session overlay starts visible'
                send('/show-s')
                until(lambda: b'/show-session' in screen, 'session command autocomplete')
                send('\t')
                pause()
                send('\r')
                until(lambda: b'[x]' in screen, 'session overlay opened from autocomplete')
                assert not (root / 'captured').exists(), 'show-session called the model'
                copy_row(2, 110, header['title'])
                copy_row(3, 110, header['id'])
                if b'\x1b[?1006h' in screen:
                    send('\x1b[<0;107;1M\x1b[<0;107;1m')
                else:
                    os.write(master, b'\x1b[M' + bytes((32, 139, 33)) +
                                     b'\x1b[M' + bytes((35, 139, 33)))
                pause(0.7)
                before = len(screen)
                send(' /show-session\r')
                until(lambda: b'[x]' in screen[before:], 'reopened session overlay')
                copy_row(3, 110, header['id'])
                send(' /new\r')
                pause(0.7)
                updated = active_header()
                assert updated['id'] != header['id']
                copy_row(3, 110, updated['id'])
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 15, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                copy_row(3, 15, updated['id'])
                print('TUI session header: title/id copy, new session and narrow resize: ok')
                return

            if case.startswith("reasoning"):
                if case == "reasoning_wait":
                    send("delegate\r")
                    until(lambda: (root / "waiting").exists(), "reasoning nested wait")
                send("draft")
                count = 0
                def step_key(key, expected):
                    nonlocal count
                    count += 1
                    send(key)
                    def changed():
                        try:
                            return json.loads((root / "effort").read_text())["count"] == count
                        except (FileNotFoundError, json.JSONDecodeError):
                            return False
                    until(changed, "effort hotkey")
                    result = json.loads((root / "effort").read_text())
                    assert result["value"] == expected, result
                    assert result["info"].get("reasoning_effort") == (None if expected == "default" else expected)
                    return result
                step_key("\x1b[1;2A", "high")
                until(lambda: b"effort high" in screen, "reasoning footer")
                step_key("\x1b[1;2A", "high")
                step_key("\x1b[1;2B", "medium")
                step_key("\x1b[1;2B", "low")
                step_key("\x1b[1;2B", "default")
                step_key("\x1b[1;2B", "default")
                if case == "reasoning":
                    send("\x1b[Z")  # Shift-Tab: plan retains its own high default.
                    result = step_key("\x1b[1;2B", "medium")
                    assert result["info"]["profile"] == "plan"
                    send("\x1b[Z")
                    result = step_key("\x1b[1;2A", "low")
                    assert result["info"]["profile"] == "implement"
                    send("\x06")  # Effort shortcuts also work with message focus.
                    step_key("\x1b[1;2A", "medium")
                    send("\x06")
                # Pasted shortcut bytes remain literal, with no effort change.
                send("\x1b[200~\x1b[1;2A\x1b[201~")
                pause()
                assert json.loads((root / "effort").read_text())["count"] == count
                if case == "reasoning_wait":
                    (root / "release").touch()
                    until(lambda: (root / "finished").exists(), "reasoning wait end")
                send(" suffix\r")
                until(lambda: (root / "captured").exists(), "draft after effort keys")
                captured = json.loads((root / "captured").read_text())
                assert captured[-1]["content"] == "draft\x1b[1;2A suffix", captured
                print(f"TUI reasoning: {case}: keys, limits, footer, draft and paste: ok")
                return

            if case.startswith("tasks"):
                send('tasks_wait\r' if case == 'tasks_wait' else 'tasks\r')
                def view_on_disk():
                    for path in (home / '.local/state/capstan/sessions').rglob('*.jsonl'):
                        header = json.loads(path.read_text().splitlines()[0])
                        if 'tasks_json' in header:
                            return header.get('tasks_view', 0)
                    return None
                if case == 'tasks_collapsed':
                    until(lambda: b'Tasks 0/12' in screen, 'configured collapsed summary')
                    assert b'TASK_01' not in screen
                    send('\x1b[1;6B')
                until(lambda: b'WRAPPED_END' in screen, 'expanded wrapped tasks')
                assert b'TASK_12' not in screen, 'overflow escaped viewport'
                title = 'Зафиксировать контракт OpenTelemetry'.encode()
                until(lambda: title in screen, 'intact Cyrillic title')
                # Fresh output after each resize, not a match from an older paint.
                for width in (80, 110):
                    before = len(screen)
                    fcntl.ioctl(master, termios.TIOCSWINSZ,
                                struct.pack('HHHH', 30, width, 0, 0))
                    proc.send_signal(signal.SIGWINCH)
                    until(lambda: title in screen[before:], 'Unicode title after resize')
                    until(lambda: b'WRAPPED_END' in screen[before:], 'wrapped title after resize')
                send('draft')
                send('\x1b[6;6~')
                until(lambda: '6–13/13'.encode() in screen, 'tasks next page')
                send('\x1b[5;6~')
                pause()
                send('\x07')  # Ctrl+G, independent of modified-arrow encoding.
                until(lambda: view_on_disk() == 1, 'persisted Ctrl+G collapse')
                until(lambda: b'Tasks 0/12' in screen, 'collapsed summary')
                send('\x07')
                until(lambda: view_on_disk() == 2, 'persisted Ctrl+G expansion')
                # Keep the old explicit bindings as compatibility aliases.
                send('\x1b[1;6A')
                until(lambda: view_on_disk() == 1, 'legacy collapse')
                send('\x1b[1;6B')
                until(lambda: view_on_disk() == 2, 'legacy expansion')
                pause()
                # Centered framed label; click its text, not the chevron.
                assert b'[ Tasks 0/12 ' in screen, 'framed summary missing'
                assert '\u256d'.encode() in screen, 'rounded panel border missing'
                if b'\x1b[?1006h' in screen:
                    send('\x1b[<0;55;17M\x1b[<0;55;17m')
                else:
                    os.write(master, b'\x1b[M' + bytes((32, 87, 49)) +
                             b'\x1b[M' + bytes((35, 87, 49)))
                until(lambda: view_on_disk() == 1, 'mouse collapse')
                pause()
                assert view_on_disk() == 1, 'mouse toggled twice'
                # The same centered text on the input border expands the panel.
                if b'\x1b[?1006h' in screen:
                    send('\x1b[<0;55;26M\x1b[<0;55;26m')
                else:
                    os.write(master, b'\x1b[M' + bytes((32, 87, 58)) +
                             b'\x1b[M' + bytes((35, 87, 58)))
                until(lambda: view_on_disk() == 2, 'mouse label expansion')
                if case == 'tasks_wait':
                    (root / 'release').touch()
                    until(lambda: (root / 'finished').exists(), 'tasks wait end')
                send(' suffix\r')
                until(lambda: (root / 'captured').exists(), 'draft after task controls')
                captured = json.loads((root / 'captured').read_text())
                assert captured[-1]['content'] == 'draft suffix', captured
                print(f'TUI tasks: {case}: default, wrapping, paging, keys, click, persistence, draft: ok')
                return

            if case.startswith("workspace_footer"):
                until(lambda: b"+128" in screen and "−34".encode() in screen, "VCS footer")
                output = bytes(screen)
                assert sgr_attributes(output.split(b"+128", 1)[0])[0] == 65
                assert sgr_attributes(output.split("−34".encode(), 1)[0])[0] == 95
                assert sgr_attributes(output.split(b"1 file", 1)[0]) == (None, False, False)
                assert root.name.encode() in output, "working directory missing"
                send("hello\r")
                until(lambda: (root / "captured").exists(), "message with footer visible")
                captured = json.loads((root / "captured").read_text())
                assert captured[-1]["content"] == "hello", "footer leaked into model input"
                before = len(screen)
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 35, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                until(lambda: b"+128" in screen[before:], "narrow footer")
                assert b"1 file" not in screen[before:], "file count did not yield to path"
                if case == "workspace_footer_custom":
                    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
                    proc.send_signal(signal.SIGWINCH)
                    pause()
                    before = len(screen)
                    send(" /footer_switch\r")
                    until(lambda: b"diff unavailable" in screen[before:], "unsupported adapter, no Git fallback")
                print(f"TUI {case}: colors, path, narrow layout and model isolation: ok")
                return

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

            if case == "markdown":
                send("markdown\r")
                until(lambda: b"MD_LAST" in screen, "Markdown message")
                output = bytes(screen)
                assert b"**MD_BOLD**" not in output and b"*MD_ITALIC*" not in output
                assert sgr_attributes(output.split(b"MD_BOLD", 1)[0])[1:] == (True, False)
                assert sgr_attributes(output.split(b"MD_PLAIN", 1)[0])[1:] == (False, False)
                italic = False
                for match in re.finditer(rb"\x1b\[([0-9;]*)m", output.split(b"MD_ITALIC", 1)[0]):
                    for value in match[1].split(b";"):
                        code = int(value or b"0")
                        if code in (0, 23): italic = False
                        elif code == 3: italic = True
                assert italic, "Markdown emphasis did not enable terminal italics"
                assert "┌".encode() in output and "└".encode() in output
                assert sgr_attributes(output.split(b"MD_CELL", 1)[0])[1:] == (True, False)
                assert b"- **MD_OLD**" in output and b"+ *MD_NEW*" in output
                assert sgr_attributes(output.split(b"- **MD_OLD**", 1)[0])[0] == 95
                assert sgr_attributes(output.split(b"+ *MD_NEW*", 1)[0])[0] == 65
                before = len(screen)
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 14, 0, 0))
                # This child has no controlling terminal; notify ncurses explicitly.
                proc.send_signal(signal.SIGWINCH)
                def narrow_fields():
                    plain = re.sub(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\([A-Z0-9])", b"", bytes(screen[before:]))
                    return b"Key: item" in plain
                until(narrow_fields, "narrow table labelled fields")
                before = len(screen)
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                until(lambda: "┌".encode() in screen[before:], "table restored after resize")
                send("capture markdown\r")
                until(lambda: (root / "captured").exists(), "raw Markdown model context")
                captured = json.loads((root / "captured").read_text())
                expected = (root / "markdown_source").read_text()
                assert any(m["role"] == "assistant" and m["content"] == expected for m in captured), captured
                print("TUI Markdown: styles, table resize, diff and raw model context: ok")
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
                assert sgr_attributes(bytes(screen).split(b"[+]", 1)[0]) == (245, True, False), \
                    (case, "expand marker is not bold gray")
                assert sgr_attributes(bytes(screen).split(b"21 lines", 1)[0]) == (245, False, False), \
                    (case, "line count should stay gray, not bold")
                assert b"BODY_01" not in screen, "folded body was painted"
                # Paste atomically: per-key paints can split 'draft' with
                # cursor moves and unrelated footer updates in the PTY stream.
                send("\x1b[200~draft\x1b[201~")
                until(lambda: b"draft" in screen, "input draft")

                # Fixed fixture layout: first manual message, or user + agent.
                # Status shares the command row; the fold marker is next.
                row = 4 if manual else 7  # Session overlay reserves no chat rows.
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
                assert sgr_attributes(prefix) == (245, False, False), \
                    (case, "shell body is not immediately plain gray")
                minus = bytes(screen).find(b"-", before)
                assert minus >= 0 and sgr_attributes(bytes(screen[:minus])) == (245, True, False), \
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
    for scenario in ("session_copy", "tasks", "tasks_wait", "tasks_collapsed", "reasoning", "reasoning_wait", "workspace_footer", "workspace_footer_custom", "markdown", "queued", "handoff", "manual", "normal", "autocomplete", "autocomplete_busy",
                     "permit_handoff", "permit_new", "shell_multiline",
                     "shell", "shell_wait", "shell_manual", "file_image", "file_image_clear"):
        run_case(scenario)
