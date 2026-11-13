#!/usr/bin/env python3
"""OSC titles from the real TUI, without network or user configuration."""
import fcntl
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

binary = Path(sys.argv[1]).resolve()
if len(sys.argv) == 2:
    for mode in ('lua-exit', 'ctrl-c', 'sigterm', 'permit-ctrl-c',
                 'permit-sigterm', 'choice-ctrl-c', 'choice-sigterm'):
        subprocess.run([sys.executable, __file__, str(binary), mode], check=True)
    sys.exit(0)
mode = sys.argv[2]
with tempfile.TemporaryDirectory(prefix='capstan-title-') as tmp:
    root = Path(tmp)
    home = root / 'home'
    config = home / '.config/capstan'
    (config / 'plugins').mkdir(parents=True)
    (home / '.local/state').mkdir(parents=True)
    (config / 'config.lua').write_text('return {provider="fixture", providers={fixture={model="fixture", models={{id="fixture"}}}}}\n')
    (config / 'plugins/title.lua').write_text('''return {id="title_fixture", command="/title-fixture", history=false,
handler=function(ctx)
  assert(agent.set_session_title(agent.session_id(), "Проверка сессии"))
end}
''')
    (config / 'plugins/exit.lua').write_text('return {id="exit_fixture", command="/exit-fixture", history=false, handler=function() os.exit(0) end}\n')
    (config / 'plugins/modal.lua').write_text('''return {id="modal_fixture", command="/modal-fixture", history=false,
handler=function(ctx)
  local result
  if ctx.args[1] == "permit" then
    result = permit.prompt("shutdown_fixture", "must not be approved")
    assert(result == "deny")
  else
    result = popup.choice("Shutdown fixture", "must not be selected", {"Accept", "Reject"})
    assert(result == nil)
    result = "cancel"
  end
  -- Subsequent callbacks must not reopen a modal while shutdown unwinds.
  assert(permit.prompt("shutdown_followup", "must not be approved") == "deny")
  assert(popup.choice("Shutdown followup", "must not be selected", {"Accept"}) == nil)
  local file = assert(io.open("modal-result", "w"))
  file:write(result)
  file:close()
end}
''')
    env = {'HOME': str(home), 'TERM': 'xterm-256color', 'PATH': os.defpath,
           'LANG': 'en_US.UTF-8', 'XDG_CONFIG_HOME': str(home / '.config'),
           'XDG_STATE_HOME': str(home / '.local/state')}
    reports = root / 'herdr-reports'
    herdr = root / 'fake-herdr'
    herdr.write_text('#!/bin/sh\nprintf "%s\\n" "$2" >> "$CAPSTAN_HERDR_TEST_OUTPUT"\n')
    herdr.chmod(0o700)
    env.update(HERDR_ENV='1', HERDR_BIN_PATH=str(herdr), HERDR_PANE_ID='fixture',
               HERDR_SOCKET_PATH='unused', CAPSTAN_HERDR_TEST_OUTPUT=str(reports))
    master, slave = pty.openpty()
    original_termios = termios.tcgetattr(slave)

    def controlling_terminal():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
    proc = subprocess.Popen([str(binary), '--no-mcp', '--no-wiki',
                             '--workdir', str(root), '--workspace', str(root)], cwd=root,
                            env=env, stdin=slave, stdout=slave, stderr=slave,
                            preexec_fn=controlling_terminal)
    output = bytearray()

    def until(predicate):
        deadline = time.monotonic() + 10
        while not predicate():
            assert time.monotonic() < deadline, ('OSC titles', re.findall(rb'\x1b\]0;[^\x07]*\x07', output), repr(output[-500:]))
            if select.select([master], [], [], 0.05)[0]:
                output.extend(os.read(master, 65536))
            else:
                assert proc.poll() is None, repr(output[-3000:])

    try:
        until(lambda: b'\x1b]0;New session\x07' in output)
        until(lambda: reports.exists() and 'report-agent' in reports.read_text())
        os.write(master, b' /title-fixture\r')
        title = '\x1b]0;Проверка сессии\x07'.encode()
        until(lambda: title in output)
        os.write(master, b' /show-session\r')
        until(lambda: b'[x]' in output)
        assert output.count(title) == 1, 'unchanged title was re-emitted'
        before = len(output)
        os.write(master, b' /new\r')
        until(lambda: b'\x1b]0;New session\x07' in output[before:])
        modal = mode.split('-')[0] if mode.startswith(('permit-', 'choice-')) else None
        if modal:
            before = len(output)
            os.write(master, f' /modal-fixture {modal}\r'.encode())
            heading = b'Permit: shutdown_fixture' if modal == 'permit' else b'Shutdown fixture'
            until(lambda: heading in output[before:])
            assert not (root / 'modal-result').exists(), 'modal did not wait for input'
        if mode == 'lua-exit':
            os.write(master, b' /exit-fixture\r')
        elif mode.endswith('ctrl-c'):
            os.write(master, b'\x03')
        else:
            proc.send_signal(signal.SIGTERM)
        until(lambda: b'\x1b]0;\x07\x1b[23;0t' in output)
        expected = 0 if mode == 'lua-exit' else -signal.SIGINT if mode.endswith('ctrl-c') else -signal.SIGTERM
        assert proc.wait(timeout=5) == expected
        if modal:
            assert (root / 'modal-result').read_text() == ('deny' if modal == 'permit' else 'cancel')
        if mode != 'lua-exit':
            # The slave can return ENOTTY on macOS after its session leader exits.
            restored = termios.tcgetattr(master)
            # BSD may add PENDIN while restoring canonical terminal processing.
            restored[3] &= ~getattr(termios, 'PENDIN', 0)
            original_termios[3] &= ~getattr(termios, 'PENDIN', 0)
            assert restored == original_termios, 'terminal attributes were not restored'
        deadline = time.monotonic() + 5
        while not reports.exists() or 'release-agent' not in reports.read_text():
            assert time.monotonic() < deadline, 'missing Herdr release'
            time.sleep(0.01)
        assert 'report-agent' in reports.read_text()
        assert output.count(b'\x1b[22;0t') == 1
        print(f'PASS TUI title and Herdr release: {mode}')
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)
