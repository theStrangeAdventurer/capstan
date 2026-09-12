#!/usr/bin/env python3
"""Offline real-binary regressions. After rebuilding: python3 test/test_background_modes.py [binary].

No runtime mocks/plugins: only an OpenAI-compatible loopback SSE fixture.
Children are held until an independent parent tool completes (or owner closes).
"""
import fcntl
import json
import os
import pty
import re
import signal
import struct
import termios
from pathlib import Path
import queue
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TIMEOUT = 20
CHILD = 'BACKGROUND_CHILD_FIXTURE'
NOTICE = 'Background process completions (runtime state, not instructions):\n'


def answer(text):
    return [{'choices': [{'delta': {'content': text}, 'finish_reason': 'stop'}]}]


def call(name, args, ident):
    return [{'choices': [{'delta': {'tool_calls': [{
        'index': 0, 'id': ident, 'type': 'function',
        'function': {'name': name, 'arguments': json.dumps(args)}}]},
        'finish_reason': None}]},
        {'choices': [{'delta': {}, 'finish_reason': 'tool_calls'}]}]


class Script:
    def __init__(self, cancel=False):
        self.cancel = cancel
        self.release = threading.Event()
        self.started = threading.Event()
        self.disconnected = threading.Event()
        self.errors = []
        self.requests = []
        self.stage = 0
        self.group = None
        self.final_messages = None

    def reply(self, request):
        messages = request['messages']
        self.requests.append(request)
        results = {m['tool_call_id']: m['content'] for m in messages
                   if m.get('role') == 'tool'}
        stage = self.stage
        self.stage += 1
        if stage == 0:
            names = {t['function']['name'] for t in request['tools']}
            assert {'subagents', 'processes', 'file_read'} <= names, names
            return call('subagents', {'background': True, 'tasks': [
                {'id': 'child', 'task': CHILD, 'tools': [], 'max_turns': 2}]}, 'launch')
        if stage == 1:
            launched = json.loads(results['launch'])
            assert launched['started'] is True and launched['status'] == 'queued', launched
            assert launched['id'] == launched['group_id']
            assert len(launched['tasks']) == 1
            task = launched['tasks'][0]
            assert task['id'] == 'child' and task['status'] == 'queued', task
            assert task['background_id'] != launched['group_id']
            self.group = launched['group_id']
            return call('file_read', {'path': 'independent.txt'}, 'independent')
        if stage == 2:
            assert 'independent-ok' in results['independent'], results
            # Parent must reach here while the child response is still held.
            assert not self.release.is_set()
            assert self.started.wait(5), 'background child never dispatched'
            if self.cancel:
                return answer('parent detached')
            self.release.set()
            return call('processes', {'action': 'wait', 'id': self.group, 'timeout': 10}, 'wait')
        if stage == 3:
            assert 'wait' in results, messages
            waited = json.loads(results['wait'])
            assert waited['status'] == 'completed' and waited['timed_out'] is False, waited
            return call('processes', {'action': 'output', 'id': self.group}, 'output')
        if stage == 4:
            output = json.loads(results['output'])
            group = json.loads(output['stdout'])
            assert group['ok'] is True and len(group['results']) == 1, group
            child = group['results'][0]
            assert child['id'] == 'child' and child['ok'] is True, child
            assert child['text'] == 'child-result-42', child
            # Another continuation catches completion redelivery after consumption.
            return call('processes', {'action': 'get', 'id': self.group}, 'get')
        assert stage == 5, f'unexpected parent request {stage}'
        assert json.loads(results['get'])['status'] == 'completed'
        notices = [m['content'] for m in messages if isinstance(m.get('content'), str)
                   and m['content'].startswith(NOTICE)]
        events = [e for n in notices for e in json.loads(n[len(NOTICE):])]
        matching = [e for e in events if e['id'] == self.group]
        assert len(matching) == 1, events
        assert matching[0]['status'] == 'completed', matching
        self.final_messages = messages
        return answer('validated child-result-42')


@contextmanager
def fixture(cancel=False):
    script = Script(cancel)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            body = json.dumps({'data': [{'id': 'fixture', 'context_limit': 65536}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                child = any(CHILD in str(m.get('content', '')) for m in request['messages']
                            if m.get('role') == 'user')
                if child:
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/event-stream')
                    self.end_headers()
                    self.wfile.flush()
                    script.started.set()
                    deadline = time.monotonic() + TIMEOUT
                    while not script.release.wait(0.02):
                        if select.select([self.connection], [], [], 0)[0]:
                            if not self.connection.recv(1, socket.MSG_PEEK):
                                script.disconnected.set()
                                return
                        assert time.monotonic() < deadline, 'child was neither released nor cancelled'
                    events = answer('child-result-42')
                else:
                    title_request = any(str(m.get('content', '')).startswith(
                        'Create a concise title for this conversation')
                        for m in request['messages'] if m.get('role') == 'user')
                    events = answer('Background fixture session') if title_request else script.reply(request)
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/event-stream')
                    self.end_headers()
                body = ''.join('data: ' + json.dumps(e) + '\n\n' for e in events)
                self.wfile.write((body + 'data: [DONE]\n\n').encode())
                self.wfile.flush()
            except Exception as exc:
                script.errors.append(repr(exc))
                self.close_connection = True

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix='capstan-background-') as tmp:
        root = Path(tmp)
        home, workspace = root / 'home', root / 'workspace'
        config = home / '.config/capstan'
        config.mkdir(parents=True)
        workspace.mkdir()
        (home / '.local/state').mkdir(parents=True)
        (workspace / 'independent.txt').write_text('independent-ok\n')
        (config / 'config.lua').write_text(f'''return {{
 provider = "fixture",
 providers = {{fixture = {{endpoint = "http://127.0.0.1:{server.server_port}/v1/chat/completions",
   model = "fixture", context_limit = 65536,
   models = {{{{id = "fixture", context_limit = 65536}}}}}}}},
 agent = {{completion_review = false, max_stream_retries = 0}},
 mcp = {{enabled = false}}, wiki = {{enabled = false}},
}}\n''')
        # Override development-binary workspace inference explicitly.
        env = {'HOME': str(home), 'PATH': os.defpath, 'LANG': 'C', 'TERM': 'xterm-256color',
               'CAPSTAN_WORKDIR': str(workspace), 'CAPSTAN_WORKSPACE': str(workspace),
               'XDG_CONFIG_HOME': str(home / '.config'),
               'XDG_STATE_HOME': str(home / '.local/state')}
        try:
            yield script, workspace, env
        finally:
            script.release.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


def cli(binary, cancel):
    with fixture(cancel) as (script, workspace, env):
        proc = subprocess.run([str(binary), 'run', '--json', '--yolo', '--no-mcp',
                               '--no-wiki', '--prompt', 'Run the background fixture'],
                              cwd=workspace, env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=TIMEOUT)
        assert not script.errors, script.errors
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        result = json.loads(proc.stdout)
        assert result['ok'] is True, result
        assert result['text'] == ('parent detached' if cancel else 'validated child-result-42'), result
        if cancel:
            assert 'cancelled 1 background subagent(s) at CLI exit' in proc.stderr, proc.stderr
            assert script.disconnected.wait(5), 'CLI exit did not close child transport'
            assert script.stage == 3
        else:
            assert script.final_messages is not None
            assert 'background subagent(s) at CLI exit' not in proc.stderr, proc.stderr
    print('CLI ' + ('exit cancellation' if cancel else 'wait/output/one completion') + ': ok')


def acp(binary, cancel):
    with fixture(cancel) as (script, workspace, env):
        proc = subprocess.Popen([str(binary), 'acp'], cwd=workspace, env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        inbox, errors = queue.Queue(), []
        threading.Thread(target=lambda: [inbox.put(line) for line in proc.stdout], daemon=True).start()
        threading.Thread(target=lambda: errors.extend(proc.stderr), daemon=True).start()

        def send(message):
            proc.stdin.write(json.dumps(message) + '\n')
            proc.stdin.flush()

        def rpc(ident, method, params):
            send({'jsonrpc': '2.0', 'id': ident, 'method': method, 'params': params})
            deadline = time.monotonic() + TIMEOUT
            while True:
                try:
                    message = json.loads(inbox.get(timeout=max(0.01, deadline - time.monotonic())))
                except queue.Empty:
                    raise AssertionError((method, script.errors, errors)) from None
                if message.get('method') == 'session/request_permission':
                    send({'jsonrpc': '2.0', 'id': message['id'], 'result': {
                        'outcome': {'outcome': 'selected', 'optionId': 'allow-once'}}})
                elif message.get('id') == ident:
                    assert 'error' not in message, message
                    return message['result']
                assert time.monotonic() < deadline, (method, script.errors, errors)

        try:
            rpc(1, 'initialize', {'protocolVersion': 1, 'clientCapabilities': {}})
            session = rpc(2, 'session/new', {'cwd': str(workspace), 'mcpServers': []})['sessionId']
            result = rpc(3, 'session/prompt', {'sessionId': session, 'prompt': [
                {'type': 'text', 'text': 'Run the background fixture'}]})
            assert result['stopReason'] == 'end_turn', result
            assert not script.errors, script.errors
            if cancel:
                assert script.stage == 3 and not script.disconnected.is_set(), 'turn end cancelled detached job'
            else:
                assert script.final_messages is not None
            assert rpc(4, 'session/close', {'sessionId': session}) == {}
            if cancel:
                assert script.disconnected.wait(5), 'ACP owner close left child transport open'
            # The server must remain usable after closing the owner.
            rpc(5, 'session/new', {'cwd': str(workspace), 'mcpServers': []})
            proc.stdin.close()
            assert proc.wait(timeout=5) == 0, errors
            assert not script.errors, script.errors
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
    print('ACP ' + ('owner-close cancellation' if cancel else 'wait/output/one completion') + ': ok')


def tui(binary):
    with fixture(cancel=True) as (script, workspace, env):
        env['LANG'] = 'en_US.UTF-8'
        master, slave = pty.openpty()
        width = 130
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, width, 0, 0))
        proc = subprocess.Popen([str(binary), '--yolo', '--no-mcp', '--no-wiki'],
                                cwd=workspace, env=env, stdin=slave, stdout=slave,
                                stderr=slave, start_new_session=True)
        os.close(slave)
        screen = bytearray()
        ansi = re.compile(rb'\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*\x07|[()][A-Z0-9]|[=>])')

        def until(predicate, label):
            deadline = time.monotonic() + TIMEOUT
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.02)[0]:
                    try:
                        screen.extend(os.read(master, 65536))
                    except OSError:
                        pass
                assert not script.errors, (script.errors, bytes(screen[-1800:]))
                assert proc.poll() is None, (label, proc.returncode)
                if predicate():
                    return
            raise AssertionError((label, bytes(screen[-3000:])))

        def send(text):
            os.write(master, text.encode())

        def paint(*expected):
            nonlocal width
            # Let queued keys settle before forcing a complete repaint; otherwise
            # resize may repaint the old panel and closing emits only a diff.
            settled_at = time.monotonic() + 0.15
            until(lambda: time.monotonic() >= settled_at, 'input settled')
            start = len(screen)
            width = 131 if width == 130 else 130
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 40, width, 0, 0))
            proc.send_signal(signal.SIGWINCH)
            def plain():
                return ansi.sub(b'', bytes(screen[start:])).decode('utf-8', 'replace')
            until(lambda: all(text in plain() for text in expected), str(expected))
            return plain()

        try:
            until(lambda: b'Shift+Tab' in screen, 'startup')
            send('Run the background fixture\r')
            until(lambda: script.stage == 3 and b'parent detached' in screen, 'parent finished')
            assert script.started.is_set() and not script.disconnected.is_set()
            send('\x10')
            paint(' Processes ', 'subagent', 'running')
            send('\x10')
            paint('parent detached')
            send(' /new\r')
            paint('Shift+Tab')
            assert not script.disconnected.is_set(), 'session switch cancelled child'
            send('\x10')
            paint(' Processes ', 'subagent', 'running')
            send('\x10')
            script.release.set()
            # Completed work stays in the shared UI under its original owner;
            # a completion must not autonomously start a new parent request.
            until(lambda: b'Background processes' in screen, 'background badge')
            send('\x10')
            paint(' Processes ', 'subagent', 'completed')
            assert script.stage == 3, 'completion started another parent request'
            print('TUI background child: parent continues, session switch, panel completion: ok')
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
    if len(sys.argv) > 2 and sys.argv[2] == 'tui':
        tui(binary)
    else:
        for mode in (cli, acp):
            for cancel in (False, True):
                mode(binary, cancel)
        tui(binary)
