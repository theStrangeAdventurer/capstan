#!/usr/bin/env python3
"""Real-binary process-panel regression; run after building: python3 test/test_process_tui.py [binary].

Uses the isolated HOME/PTY practices of test_tui_wait_input.py. That harness
keeps its helpers nested in run_case, so this small harness is standalone.
Only fixture files are read; no inherited credentials, provider, or network.
"""
import fcntl
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

ANSI = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*\x07|[()][A-Z0-9]|[=>])")
PLUGIN = r'''
local json = require('vendor.rxi.json')
_G.should_auto_compact = function() return false end
_G.agent_entry = function(messages)
  local f = assert(io.open('captured', 'w'))
  f:write(json.encode(messages))
  f:close()
  agent.finish_run()
end
return {id = 'process_tui_fixture'}
'''


def run(binary):
    Path('build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tui-process-', dir='build') as tmp:
        root = Path(tmp).resolve()
        home = root / 'home'
        config = home / '.config/capstan'
        (config / 'plugins').mkdir(parents=True)
        (home / '.local/state').mkdir(parents=True)
        (config / 'config.lua').write_text('''return {
  provider = "fixture",
  providers = {fixture = {model = "fixture", context_limit = 4096,
    models = {{id = "fixture", context_limit = 4096}}}},
}\n''')
        (config / 'plugins/process_tui_fixture.lua').write_text(PLUGIN)
        env = {'HOME': str(home), 'TERM': 'xterm-256color',
               'PATH': os.defpath, 'LANG': 'en_US.UTF-8',
               'XDG_CONFIG_HOME': str(home / '.config'),
               'XDG_STATE_HOME': str(home / '.local/state')}
        master, slave = pty.openpty()
        width = 110
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, width, 0, 0))
        proc = subprocess.Popen([str(binary), '--no-mcp', '--no-wiki',
                                 '--workdir', str(root), '--workspace', str(root)],
                                stdin=slave, stdout=slave, stderr=slave,
                                cwd=root, env=env, start_new_session=True)
        os.close(slave)
        screen = bytearray()

        def send(text):
            os.write(master, text.encode())

        def until(predicate, label):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.02)[0]:
                    try:
                        screen.extend(os.read(master, 65536))
                    except OSError:
                        pass
                if predicate():
                    return
                if proc.poll() is not None:
                    raise AssertionError(f'Capstan exited during {label}: {proc.returncode}')
            raise AssertionError(f'Timeout: {label}\nPTY tail: {bytes(screen[-4000:])!r}')

        def plain(start=0):
            return ANSI.sub(b'', bytes(screen[start:])).decode('utf-8', 'replace')

        def paint(*expected):
            # A real resize forces a complete ncurses paint, avoiding assertions
            # against stale history or strings split by incremental cursor edits.
            nonlocal width
            start = len(screen)
            width = 111 if width == 110 else 110
            fcntl.ioctl(master, termios.TIOCSWINSZ,
                        struct.pack('HHHH', 40, width, 0, 0))
            proc.send_signal(signal.SIGWINCH)
            until(lambda: all(text in plain(start) for text in expected),
                  'fresh paint: ' + ', '.join(expected))
            return plain(start)

        def alive(pid):
            try:
                os.kill(pid, 0)
                return True
            except ProcessLookupError:
                return False

        def pid_from(name):
            path = root / name
            until(lambda: path.exists() and path.read_text().strip().isdigit(), name)
            return int(path.read_text())

        def capture(expected):
            path = root / 'captured'
            def matches():
                try:
                    messages = json.loads(path.read_text())
                except (FileNotFoundError, json.JSONDecodeError):
                    return False
                return messages[-1]['content'] == expected
            until(matches, 'preserved draft: ' + expected)
            path.unlink()

        try:
            until(lambda: b'Shift+Tab' in screen, 'startup')
            assert 'Background processes:' not in paint('Shift+Tab')
            # The label deliberately does not contain the contiguous output
            # marker "ready", so matching it in details proves output is live.
            command = "printf 'rea%s\\n' dy; echo $$ > background.pid; sleep 60"
            send(' /shell --background ' + command + '\r')
            background = pid_from('background.pid')
            until(lambda: b'ctx:shell' in screen, 'background command returned')
            send('\x1b[200~draft\x1b[201~')
            until(lambda: b'draft' in screen, 'draft input')
            paint('[ Background processes: 1 | /processes for details ]')
            if b'\x1b[?1006h' in screen:
                send(f'\x1b[<0;{width - 3};1M\x1b[<0;{width - 3};1m')
            else:
                os.write(master, b'\x1b[M' + bytes((32, 32 + width - 3, 33)) +
                         b'\x1b[M' + bytes((35, 32 + width - 3, 33)))
            paint(' Processes ', 'PID', 'KIND', 'OWNER', 'ELAPSED',
                  'STATE', str(background), 'running')
            send('\r')
            paint('PID ' + str(background), 'running', 'Owner:', 'Workdir:',
                  "printf 'rea%s", 'stdout', 'ready')
            assert alive(background), 'output only became visible after exit'
            send('s')
            paint('Stop this process group? [y/N]')
            send('n')
            # A resize can race the queued key: first acknowledge the return
            # to details, then inspect a fresh frame for absence of the modal.
            paint('stdout', 'ready', 'running')
            text = paint('stdout', 'ready', 'running')
            assert 'Stop this process group?' not in text
            assert alive(background), 'n stopped the process'
            send('s')
            paint('Stop this process group? [y/N]')
            send('y')
            until(lambda: not alive(background), 'confirmed stop reaped child')
            paint('exited', 'PID ' + str(background))
            send('\x10')
            assert 'Background processes:' not in paint('draft')
            send(' suffix\r')
            capture('draft suffix')

            # The hidden indicator must no longer accept clicks.
            assert 'Background processes:' not in paint('INSERT')
            if b'\x1b[?1006h' in screen:
                send(f'\x1b[<0;{width - 3};1M\x1b[<0;{width - 3};1m')
            else:
                os.write(master, b'\x1b[M' + bytes((32, 32 + width - 3, 33)) +
                         b'\x1b[M' + bytes((35, 32 + width - 3, 33)))
            assert 'Esc: back/close' not in paint('INSERT')
            # Completed entries remain accessible through the hotkey.
            send('\x10')
            paint(' Processes ', 'Esc: back/close', str(background))
            send('\x10')
            assert 'Background processes:' not in paint('INSERT')

            # This command blocks in the real synchronous shell adapter, not
            # a Lua imitation of its wait loop. A marker releases it in finally.
            send(" /shell echo $$ > sync.pid; printf 'sync_%s\\n' ready; "
                 "while [ ! -f release ]; do sleep 0.05; done\r")
            synchronous = pid_from('sync.pid')
            assert alive(synchronous)
            send(' /processes\r')
            paint(' Processes ', 'Esc: back/close')
            assert alive(synchronous), '/processes waited for synchronous completion'
            # Hotkey closes regardless of retained list/detail selection state.
            send('\x10')
            paint('[ Background processes: 1 | /processes for details ]', 'INSERT')
            send('\x1b[200~sync draft\x1b[201~')
            until(lambda: b'sync draft' in screen, 'synchronous draft')
            send('\x10')
            paint(' Processes ', 'Esc: back/close')
            send('\x10')
            paint('sync draft')
            (root / 'release').touch()
            until(lambda: not alive(synchronous), 'synchronous completion')
            send(' suffix\r')
            capture('sync draft suffix')
            print('TUI processes: background, live output, PID/status/labels, stop n/y, '
                  'drafts, mouse and synchronous /processes: ok')
        finally:
            (root / 'release').touch()
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)


if __name__ == '__main__':
    run(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/capstan').resolve())
