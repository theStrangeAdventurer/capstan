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
                 'permit-sigterm', 'choice-ctrl-c', 'choice-sigterm',
                 'permit-accept', 'permit-reject', 'choice-accept', 'choice-cancel',
                 'permit-working', 'choice-working', 'resume'):
        subprocess.run([sys.executable, __file__, str(binary), mode], check=True)
    sys.exit(0)
mode = sys.argv[2]
with tempfile.TemporaryDirectory(prefix='capstan-title-') as tmp:
    root = Path(tmp).resolve()
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
  if ctx.args[3] == "working" then agent.set_running(true) end
  if ctx.args[1] == "permit" then
    result = permit.prompt("shutdown_fixture", "must not be approved")
    assert(result == (ctx.args[2] == "accept" and "allow" or "deny"))
  else
    result = popup.choice("Shutdown fixture", "must not be selected", {"Accept", "Reject"})
    assert(result == (ctx.args[2] == "accept" and "Accept" or nil))
    result = result or "cancel"
  end
  if not ctx.args[2] then
    -- Subsequent callbacks must not reopen a modal while shutdown unwinds.
    assert(permit.prompt("shutdown_followup", "must not be approved") == "deny")
    assert(popup.choice("Shutdown followup", "must not be selected", {"Accept"}) == nil)
  end
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
    herdr.write_text('#!/bin/sh\nprintf "%s\\n" BEGIN "$@" END >> "$CAPSTAN_HERDR_TEST_OUTPUT"\n')

    def records():
        if not reports.exists():
            return []
        result = []
        for block in reports.read_text().split('BEGIN\n')[1:]:
            if block.endswith('END\n'):
                result.append(block[:-4].splitlines())
        return result

    def states():
        return [r[r.index('--state') + 1] for r in records() if '--state' in r]
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
                             '--max-turns', '17', '--reasoning-effort', 'low',
                             '--no-preserve-reasoning',
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
        previous_session = records()[-1][records()[-1].index('--agent-session-id') + 1]
        before = len(output)
        os.write(master, b' /new\r')
        until(lambda: b'\x1b]0;New session\x07' in output[before:])
        until(lambda: any('--agent-session-id' in r and
                          r[r.index('--agent-session-id') + 1] != previous_session
                          for r in records()))
        modal = mode.split('-')[0] if mode.startswith(('permit-', 'choice-')) else None
        working = mode.endswith('-working')
        decision = 'accept' if working else mode.split('-')[1] if mode.endswith(('-accept', '-reject', '-cancel')) else None
        if modal:
            before = len(output)
            os.write(master, f' /modal-fixture {modal} {decision or ""} {"working" if working else ""}\r'.encode())
            heading = b'Permit: shutdown_fixture' if modal == 'permit' else b'Shutdown fixture'
            until(lambda: heading in output[before:])
            until(lambda: states()[-1:] == ['blocked'])
            assert not (root / 'modal-result').exists(), 'modal did not wait for input'
            if decision:
                os.write(master, b'\r' if decision == 'accept' else b'\x1b')
                until(lambda: (root / 'modal-result').exists())
                until(lambda: states()[-1:] == ['working' if working else 'idle'])
                assert (root / 'modal-result').read_text() == (
                    'allow' if modal == 'permit' and decision == 'accept' else
                    'Accept' if decision == 'accept' else
                    'deny' if modal == 'permit' else 'cancel')
        resume_record = next(r for r in (records() if mode == 'resume' else reversed(records())) if '--' in r)
        resume = resume_record[resume_record.index('--') + 1:]
        assert resume[0] == 'capstan'
        for flag, value in (('--session-id', resume_record[resume_record.index('--agent-session-id') + 1]),
                            ('--workdir', str(root)), ('--workspace', str(root)),
                            ('--provider', 'fixture'), ('--model', 'fixture'),
                            ('--max-turns', '17'), ('--reasoning-effort', 'low')):
            assert resume[resume.index(flag) + 1] == value, resume
        assert all(flag in resume for flag in ('--no-mcp', '--no-wiki', '--no-preserve-reasoning'))
        if working:
            proc.send_signal(signal.SIGINT)
        elif mode == 'lua-exit' or decision or mode == 'resume':
            os.write(master, b' /exit-fixture\r')
        elif mode.endswith('ctrl-c'):
            os.write(master, b'\x03')
        else:
            proc.send_signal(signal.SIGTERM)
        until(lambda: b'\x1b]0;\x07\x1b[23;0t' in output)
        expected = -signal.SIGINT if working else 0 if mode == 'lua-exit' or decision or mode == 'resume' else -signal.SIGINT if mode.endswith('ctrl-c') else -signal.SIGTERM
        assert proc.wait(timeout=5) == expected
        if modal and not decision:
            assert (root / 'modal-result').read_text() == ('deny' if modal == 'permit' else 'cancel')
        if expected != 0:
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
        if mode == 'resume':
            # The durable active pointer now names a different session. Execute
            # the reported command via PATH and verify explicit session restore.
            bindir = root / 'bin'
            bindir.mkdir()
            (bindir / 'capstan').symlink_to(binary)
            env['PATH'] = str(bindir) + os.pathsep + os.defpath
            reports.unlink()
            output.clear()
            # Allocate before closing the old PTY so macOS cannot reuse a tty
            # that is still tearing down its previous controlling session.
            old_master, old_slave = master, slave
            master, slave = pty.openpty()
            os.close(old_master)
            os.close(old_slave)
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
            proc = subprocess.Popen(resume, cwd=root, env=env, stdin=slave,
                                    stdout=slave, stderr=slave,
                                    preexec_fn=controlling_terminal)
            until(lambda: title in output)
            until(lambda: any('--agent-session-id' in r and
                              r[r.index('--agent-session-id') + 1] == previous_session
                              for r in records()))
            os.write(master, b'\x03')
            until(lambda: b'\x1b]0;\x07\x1b[23;0t' in output)
            assert proc.wait(timeout=5) == -signal.SIGINT
        print(f'PASS TUI title and Herdr lifecycle: {mode}')
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        os.close(master)
        os.close(slave)
