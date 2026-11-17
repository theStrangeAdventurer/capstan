#!/usr/bin/env python3
"""Verify live selection on the real PTY, before sending button release."""
import fcntl
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

ANSI = re.compile(rb'\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*\x07|[()][A-Z0-9]|[=>])')


def reversed_text(data):
    reverse = False
    result = bytearray()
    pos = 0
    for match in ANSI.finditer(data):
        if reverse:
            result.extend(data[pos:match.start()])
        seq = match.group()
        if seq.startswith(b'\x1b[') and seq.endswith(b'm'):
            for value in seq[2:-1].split(b';'):
                if value in (b'', b'0', b'27'):
                    reverse = False
                elif value == b'7':
                    reverse = True
        pos = match.end()
    if reverse:
        result.extend(data[pos:])
    return bytes(result)


def run(binary, term):
    with tempfile.TemporaryDirectory(prefix='mouse-', dir='build') as tmp:
        root = Path(tmp).resolve()
        config = root / '.config/capstan'
        (config / 'plugins').mkdir(parents=True)
        (root / '.local/state').mkdir(parents=True)
        (config / 'config.lua').write_text('return {}\n')
        (config / 'plugins/fixture.lua').write_text('''
_G.agent_entry = function() agent.finish_run() end
return {id = 'mouse_fixture'}
''')
        env = dict(HOME=str(root), TERM=term, PATH=os.defpath,
                   LANG='en_US.UTF-8', XDG_CONFIG_HOME=str(root / '.config'),
                   XDG_STATE_HOME=str(root / '.local/state'))
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
        proc = subprocess.Popen([str(binary), '--no-mcp', '--no-wiki',
                                 '--workdir', str(root), '--workspace', str(root)],
                                cwd=root, env=env, stdin=slave, stdout=slave,
                                stderr=slave, start_new_session=True)
        os.close(slave)
        screen = bytearray()

        def drain(seconds=0.3):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], 0.02)[0]:
                    screen.extend(os.read(master, 65536))

        def until(predicate, label='condition'):
            end = time.monotonic() + 8
            while time.monotonic() < end:
                drain(0.05)
                if predicate():
                    return
            raise AssertionError(f'{label}: ' + repr(bytes(screen[-1500:])))

        def mouse(button, x, release=False):
            if b'\x1b[?1006h' in screen or b'\x1b[?1006;1000h' in screen:
                os.write(master, f'\x1b[<{button};{x};3{"m" if release else "M"}'.encode())
            else:
                os.write(master, b'\x1b[M' + bytes((35 if release else 32 + button, 32 + x, 35)))

        try:
            until(lambda: b'Shift+Tab' in screen)
            assert b'\x1b[?1002h' in screen, 'button-motion reporting not enabled'
            os.write(master, b'abcdefghijklmnop\r')
            until(lambda: b'abcdefghijklmnop' in screen)
            drain()
            mouse(0, 3)  # First character of the first message, row 3.
            drain()
            start = len(screen)
            mouse(32, 10)
            until(lambda: b'bcdefg' in reversed_text(bytes(screen[start:])))
            assert b'Text copied' not in screen, 'copied before release'
            # A second movement must keep the original anchor, including backwards.
            start = len(screen)
            mouse(32, 6)
            drain()
            # Force a full paint only to inspect the resulting range after contraction.
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 101, 0, 0))
            import signal
            proc.send_signal(signal.SIGWINCH)
            until(lambda: b'abcd' in reversed_text(bytes(screen[start:])))
            assert b'efgh' not in reversed_text(bytes(screen[start:])), 'range did not contract'
            mouse(0, 6, release=True)
            until(lambda: b'Text copied' in screen)
            print(f'{term}: live drag, retained anchor, contraction, release: ok')
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)


def run_resume(binary, term):
    """Drag mode 1002 must be re-asserted after SIGTSTP resume and /editor."""
    import signal
    with tempfile.TemporaryDirectory(prefix='mouse-resume-', dir='build') as tmp:
        root = Path(tmp).resolve()
        config = root / '.config/capstan'
        (config / 'plugins').mkdir(parents=True)
        (root / '.local/state').mkdir(parents=True)
        (config / 'config.lua').write_text('return {}\n')
        (config / 'plugins/fixture.lua').write_text('''
_G.agent_entry = function() agent.finish_run() end
return {id = 'mouse_fixture'}
''')
        env = dict(HOME=str(root), TERM=term, PATH=os.defpath,
                   LANG='en_US.UTF-8', XDG_CONFIG_HOME=str(root / '.config'),
                   XDG_STATE_HOME=str(root / '.local/state'), EDITOR='true')
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
        proc = subprocess.Popen([str(binary), '--no-mcp', '--no-wiki',
                                 '--workdir', str(root), '--workspace', str(root)],
                                cwd=root, env=env, stdin=slave, stdout=slave,
                                stderr=slave, start_new_session=True)
        os.close(slave)
        screen = bytearray()

        def drain(seconds=0.3):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], 0.02)[0]:
                    screen.extend(os.read(master, 65536))

        def until(predicate, label='condition'):
            end = time.monotonic() + 8
            while time.monotonic() < end:
                drain(0.05)
                if predicate():
                    return
            raise AssertionError(f'{label}: ' + repr(bytes(screen[-1500:])))

        def mouse(button, x, release=False):
            if b'\x1b[?1006h' in screen or b'\x1b[?1006;1000h' in screen:
                os.write(master, f'\x1b[<{button};{x};3{"m" if release else "M"}'.encode())
            else:
                os.write(master, b'\x1b[M' + bytes((35 if release else 32 + button, 32 + x, 35)))

        def drag_and_copy():
            mark = len(screen)
            mouse(0, 3)
            drain(0.25)
            mouse(32, 10)
            drain(0.25)
            mouse(0, 10, release=True)
            until(lambda: b'Text copied' in screen[mark:], 'copy on release')

        try:
            until(lambda: b'Shift+Tab' in screen)
            startup = screen.count(b'\x1b[?1002h')
            assert startup >= 1, 'startup did not enable drag mode'

            os.write(master, b'abcdefghijklmnop\r')
            until(lambda: b'abcdefghijklmnop' in screen)
            drain(0.7)
            drag_and_copy()

            # Ctrl+Z suspend, then fg: ncurses re-enables only mode 1000.
            proc.send_signal(signal.SIGTSTP)
            drain(0.6)
            proc.send_signal(signal.SIGCONT)
            drain(1.0)
            assert screen.count(b'\x1b[?1002h') > startup, \
                'drag mode not re-asserted after SIGTSTP resume'
            drain(0.8)
            drag_and_copy()

            # /editor shells out through endwin; the picker needs two Enters.
            # Let the compact "Copied" popup close, then leave the visual mode
            # the drags left behind: first Esc clears the selection, the second
            # returns focus to the input box.
            drain(0.8)
            os.write(master, b'\x1b')
            drain(0.3)
            os.write(master, b'\x1b')
            drain(0.3)
            before_editor = screen.count(b'\x1b[?1002h')
            os.write(master, b'/')
            drain(0.8)
            os.write(master, b'editor')
            drain(0.6)
            os.write(master, b'\r')
            drain(1.0)
            os.write(master, b'\r')
            drain(2.0)
            assert screen.count(b'\x1b[?1002h') > before_editor, \
                'drag mode not re-asserted after /editor'
            drag_and_copy()
            print(f'{term}: drag mode re-asserted after suspend and editor: ok')
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)


if __name__ == '__main__':
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/capstan').resolve()
    for term in ('xterm-256color', 'screen'):
        run(binary, term)
        run_resume(binary, term)
