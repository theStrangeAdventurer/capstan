#!/usr/bin/env python3
"""Real ncurses popup mouse regression; fake URL and desktop commands only."""
import fcntl
import http.server
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from test_mouse_selection import reversed_text


def run(binary, term, blocking):
    with tempfile.TemporaryDirectory(prefix='popup-mouse-', dir='build') as tmp:
        root = Path(tmp).resolve()
        config = root / '.config/capstan'
        (config / 'plugins').mkdir(parents=True)
        (root / 'bin').mkdir()
        (root / '.local/state').mkdir(parents=True)
        (config / 'config.lua').write_text('return {}\n')
        for name in ('open', 'xdg-open'):
            path = root / 'bin' / name
            path.write_text('#!/bin/sh\nprintf "%s" "$1" > "$HOME/opened"\n')
            path.chmod(0o700)
        for name in ('pbcopy', 'wl-copy', 'xclip', 'xsel'):
            path = root / 'bin' / name
            path.write_text('#!/bin/sh\ncat > "$HOME/copied"\n')
            path.chmod(0o700)
        url = 'https://example.test/auth?state=' + 'a' * 120 + '&nonce=last%20value'
        body = 'Continue:\n' + url + '\n' + '\n'.join('line%02d' % i for i in range(30))
        release_http = threading.Event()

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                release_http.wait(15)
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'ok')

            def log_message(self, *_):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        wait = "http.get('http://127.0.0.1:%d/')" % server.server_port if blocking else ''
        # A long popup exercises clipping with a scrollbar, while HTTP pumps the
        # same blocking UI path used by the OAuth callback listener.
        (config / 'plugins/fixture.lua').write_text('''
_G.agent_entry = function()
  local f = assert(io.open(os.getenv('HOME') .. '/entered', 'w')); f:write('yes'); f:close()
  popup.info('Popup mouse fixture', [=[%s]=])
  %s
  agent.finish_run()
end
return {id = 'popup_mouse_fixture'}
''' % (body, wait))
        env = dict(HOME=str(root), TERM=term, PATH=str(root / 'bin') + ':' + os.defpath,
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

        def drain(seconds=0.2):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], 0.02)[0]:
                    screen.extend(os.read(master, 65536))

        def until(predicate):
            end = time.monotonic() + 8
            while time.monotonic() < end:
                drain(0.05)
                if predicate():
                    return
            Path('build/popup-mouse-failure.log').write_bytes(screen)
            raise AssertionError('entered=' + str((root / 'entered').exists()) + ': ' + repr(bytes(screen[-2000:])))

        def mouse(button, x, y, release=False):
            if b'\x1b[?1006h' in screen or b'\x1b[?1006;1000h' in screen:
                value = f'\x1b[<{button};{x};{y}{"m" if release else "M"}'.encode()
            else:
                value = b'\x1b[M' + bytes((35 if release else 32 + button, 32 + x, 32 + y))
            os.write(master, value)
            drain()

        def copied(expected):
            return (root / 'copied').exists() and (root / 'copied').read_text() == expected

        try:
            until(lambda: b'Shift+Tab' in screen)
            os.write(master, b'fixture\r')
            until(lambda: b'Popup mouse fixture' in screen)
            drain()
            # At 100 columns: popup x=1, y=1, body x=3, y=3 (zero based),
            # text width=94. Click the URL continuation, not its scheme.
            mouse(0, 12, 6)
            mouse(0, 12, 6, True)
            until(lambda: (root / 'opened').exists())
            assert (root / 'opened').read_text() == url
            # Select entire URL across its soft wrap. No copy before release.
            mouse(0, 4, 5)
            mark = len(screen)
            mouse(32, 4 + len(url) - 94, 6)
            assert not (root / 'copied').exists()
            assert b'aaaa' in reversed_text(bytes(screen[mark:]))
            mouse(0, 4 + len(url) - 94, 6, True)
            until(lambda: copied(url))
            # Backward selection retains the same raw URL.
            mouse(0, 4 + len(url) - 94, 6)
            mouse(32, 4, 5)
            mouse(0, 4, 5, True)
            until(lambda: copied(url))
            # Scroll one wrapped row down; URL hit testing follows source offsets.
            (root / 'opened').unlink()
            os.write(master, b'j')
            drain()
            mouse(0, 12, 5)
            mouse(0, 12, 5, True)
            until(lambda: (root / 'opened').exists())
            assert (root / 'opened').read_text() == url
            # Copy all includes real newlines but never layout-created ones.
            mouse(0, 4, 4)
            mouse(0, 4, 4, True)
            os.write(master, b'c')
            until(lambda: copied(body))
            print(f'{term}, blocking={blocking}: wrapped URL click, live forward/backward drag, scroll, copy: ok')
        finally:
            release_http.set()
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/capstan').resolve()
    for term in ('xterm-256color', 'screen'):
        for blocking in (False, True):
            run(binary, term, blocking)
