#!/usr/bin/env python3
"""Actual ncurses /connect regression; run after rebuilding embedded Lua assets.

Usage: python3 test/test_connect_pty.py [build/capstan]
Only fake keys and native file storage in isolated HOME/XDG; no network or Keychain.
"""
import fcntl
import json
import stat
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
SECRET = "fixture-connect-not-a-real-key-7391"
CANCELLED_SECRET = "fixture-cancel-not-a-real-key-2846"
PLUGIN = r'''
local expected = string.char(FIXTURE_SECRET_BYTES)
local function mark(name, value)
  local f = assert(io.open(name, "w"))
  f:write(tostring(value))
  f:close()
end
-- Leave capstan.oauth.store_open untouched: exercise native writes and modes.
-- Tripwire only: obsolete credential paths must never touch the real Keychain.
local function forbidden_keychain()
  mark("unexpected_keychain", true)
  error("fixture forbids Keychain")
end
capstan.keychain = {available = false, get = forbidden_keychain,
  set = forbidden_keychain, remove = forbidden_keychain}
mark("credential_dir", capstan.state_path("credentials"))
mark("endpoint", require("agent.credentials").definition("opencode_go").endpoint)
-- Fail closed if a command accidentally dispatches to the agent/provider.
_G.should_auto_compact = function() return false end
_G.agent_entry = function()
  mark("unexpected_agent", true)
  agent.finish_run()
end
http.get = function() mark("unexpected_http", true); error("fixture forbids HTTP") end
http.post = http.get
http.post_stream = http.get
mark("ready", true)
return {id = "connect_fixture", command = "/verify", history = false,
  auth = {provider = "fixture_oauth", label = "Fixture OAuth",
    authorize = function() mark("unexpected_oauth", true); return nil, "fixture only" end},
  handler = function()
    local credentials = require("agent.credentials")
    local endpoint = credentials.definition("opencode_go").endpoint
    local key, source = credentials.resolve("opencode_go", {endpoint = endpoint}, endpoint)
    mark("verified", key == expected and type(source) == "string"
      and source:lower():match("^file") ~= nil)
    return "Fixture verified"
  end}
'''


def run_case(mode):
    with tempfile.TemporaryDirectory(prefix="connect-pty-", dir="build") as tmp:
        root = Path(tmp).resolve()
        home = root / "home"
        config = home / ".config/capstan"
        (config / "plugins").mkdir(parents=True)
        (home / ".local/state").mkdir(parents=True)
        (home / ".cache").mkdir()
        (config / "config.lua").write_text('''return {
  provider = "fixture",
  providers = {fixture = {model = "fixture", context_limit = 4096,
    models = {{id = "fixture", context_limit = 4096}}}},
}
''')
        (config / "plugins/connect_fixture.lua").write_text(
            PLUGIN.replace("FIXTURE_SECRET_BYTES", ",".join(str(ord(c)) for c in SECRET)))
        env = {"HOME": str(home), "TERM": "xterm-256color", "PATH": os.defpath,
               "LANG": "en_US.UTF-8", "XDG_CONFIG_HOME": str(home / ".config"),
               "XDG_STATE_HOME": str(home / ".local/state"),
               "XDG_CACHE_HOME": str(home / ".cache"),
               "CAPSTAN_DISABLE_CLIPBOARD": "1"}
        def launch():
            (root / "ready").unlink(missing_ok=True)
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
            try:
                proc = subprocess.Popen(
                    [str(BINARY), "--no-mcp", "--no-wiki", "--workdir", str(root),
                     "--workspace", str(root)], stdin=slave, stdout=slave, stderr=slave,
                    env=env, cwd=root, start_new_session=True)
            finally:
                os.close(slave)
            return proc, master

        proc, master = launch()
        screen = bytearray()

        def stop():
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            drain()
            os.close(master)

        def check_files(credential_file):
            for path in root.rglob("*"):
                if path.is_file() and path != credential_file:
                    data = path.read_bytes()
                    assert all(key.encode() not in data for key in (SECRET, CANCELLED_SECRET)), \
                        f"{mode}: fake secret leaked outside credential file"

        def check_saved(credential_file):
            assert stat.S_IMODE(credential_file.parent.stat().st_mode) == 0o700, \
                "credential directory must be 0700"
            assert stat.S_IMODE(credential_file.stat().st_mode) == 0o600, \
                "credential file must be 0600"
            try:
                data = json.loads(credential_file.read_bytes())
            except (ValueError, UnicodeError):
                raise AssertionError("invalid credential JSON") from None
            assert data == {"version": 1, "providers": {"opencode_go": {
                "endpoint": (root / "endpoint").read_text(), "api_key": SECRET}}}, \
                "unexpected credential schema or value"
            check_files(credential_file)

        def drain():
            if select.select([master], [], [], 0.02)[0]:
                try:
                    screen.extend(os.read(master, 65536))
                except OSError:
                    pass
            # Do not print terminal output even on failure.
            plain = re.sub(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\([A-Z0-9])", b"", screen)
            for key in (SECRET, CANCELLED_SECRET):
                assert key.encode() not in screen and key.encode() not in plain, \
                    f"{mode}: raw fake secret leaked to terminal"
            for name in ("agent", "http", "oauth", "keychain"):
                assert not (root / ("unexpected_" + name)).exists(), \
                    f"{mode}: unexpected {name} call"

        def until(predicate, label):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                drain()
                if predicate():
                    return
                assert proc.poll() is None, f"{mode}: process exited during {label}"
            raise AssertionError(f"{mode}: timeout during {label}")

        def pause():
            deadline = time.monotonic() + 0.25
            while time.monotonic() < deadline:
                drain()

        def send(text):
            os.write(master, text.encode())

        def seen(text, start=0):
            until(lambda: text.encode() in screen[start:], text)

        def marker(name, expected):
            path = root / name
            until(lambda: path.exists() and path.read_text() == expected, name)

        def open_secret(direct=False):
            start = len(screen)
            if direct:
                send(" /connect opencode_go\r")
                seen("Enter API key", start)
                return
            send(" /connect\r")  # Avoid slash autocomplete.
            seen("Choose a provider", start)
            for label in ("OpenCode Go", "ChatGPT / Codex"):
                seen(label, start)
            for label in (b"DeepSeek", b"OpenRouter", b"Fixture OAuth", b"openai_codex"):
                assert label not in screen[start:], "unexpected provider in connect picker"
            # Go is first; keep native choice's label return.
            send("\r")
            seen("Enter API key", start)

        def verify():
            (root / "verified").unlink(missing_ok=True)
            start = len(screen)
            send(" /verify\r")
            marker("verified", "true")
            seen("Fixture verified", start)
            send("\r")
            pause()

        try:
            marker("ready", "true")
            seen("\x1b[?2004h")
            seen("Shift+Tab")
            credential_dir = Path((root / "credential_dir").read_text()).resolve()
            assert credential_dir.is_relative_to(root), "credential path escaped fixture"
            credential_file = credential_dir / "state.json"
            original = b'{"version":1,"providers":BROKEN}\n'
            if mode == "corrupt":
                credential_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
                credential_file.write_bytes(original)
                credential_file.chmod(0o600)
            open_secret(direct=mode != "saved")
            send("\x1b[200~" + SECRET + "\x1b[201~")
            pause()
            assert (credential_file.read_bytes() == original if mode == "corrupt"
                    else not credential_file.exists()), "paste submitted without Enter"
            start = len(screen)
            send("\r")
            if mode == "corrupt":
                until(lambda: any(word in screen[start:].lower() for word in
                      (b"failed", b"error", b"invalid", b"corrupt", b"unavailable")),
                      "credential storage error")
                pause()
                assert b"Connected: opencode_go" not in screen[start:], \
                    "corrupt storage reported success"
                assert credential_file.read_bytes() == original, "corrupt file overwritten"
                assert stat.S_IMODE(credential_dir.stat().st_mode) == 0o700
                assert stat.S_IMODE(credential_file.stat().st_mode) == 0o600
            else:
                seen("Connected: opencode_go", start)
                send("\r")  # Dismiss no-history status popup.
                pause()
                verify()
                check_saved(credential_file)
                original = credential_file.read_bytes()
                if mode == "saved":
                    stop()
                    start = len(screen)
                    proc, master = launch()
                    marker("ready", "true")
                    seen("Shift+Tab", start)
                    verify()  # No /connect in the restarted process.
                else:
                    open_secret()  # Direct argument and picker share the secret flow.
                    send("\x1b[200~" + CANCELLED_SECRET + "\x1b[201~")
                    pause()
                    start = len(screen)
                    send("\x1b")
                    seen("Connection cancelled", start)
                    send("\r")
                    pause()
                    verify()
                assert credential_file.read_bytes() == original, "existing key changed"
                check_saved(credential_file)
            drain()
        finally:
            stop()
        check_files(credential_file)  # Includes logs/session state flushed at exit.
        assert credential_file.read_bytes() == original, "credential file changed at exit"
        print(f"connect PTY {mode}: native file storage: ok")


if __name__ == "__main__":
    for scenario in ("saved", "cancel", "corrupt"):
        run_case(scenario)
