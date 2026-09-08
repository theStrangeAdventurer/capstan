#!/usr/bin/env python3
"""Real-binary CLI/TUI OTLP checks; only disposable workspace files and loopback.

Run: make build/capstan && python3 test/test_telemetry_modes.py [build/capstan]
ACP is deliberately not covered here. No user configuration/environment is read.
"""
import collections
import contextlib
import fcntl
import http.server
import json
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
import unittest

# The shared parser consumes a positional driver argument at import time.
_saved_argv = sys.argv
try:
    sys.argv = [sys.argv[0]]
    from test_telemetry_otlp import receiver, decode, one, wire, attributes
finally:
    sys.argv = _saved_argv
BINARY = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 and
                  not sys.argv[1].startswith('-') else 'build/capstan').resolve())
ANSWER = 'FIXTURE_ANSWER_DONE'
PROMPT = 'FIXTURE_PROMPT_PRIVATE'


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(request)
        has_tool = any(m.get('role') == 'tool' for m in request['messages'])
        delta = {'content': ANSWER} if has_tool else {'tool_calls': [{
            'index': 0, 'id': 'fixture_call', 'type': 'function',
            'function': {'name': 'file_read', 'arguments': '{"path":"fixture.txt"}'}}]}
        chunks = [{'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]},
                  {'choices': [{'index': 0, 'delta': {},
                                'finish_reason': 'stop' if has_tool else 'tool_calls'}],
                   'usage': {'prompt_tokens': 10, 'completion_tokens': 2, 'total_tokens': 12}}]
        body = ''.join('data: ' + json.dumps(c) + '\n\n' for c in chunks)
        body = (body + 'data: [DONE]\n\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


@contextlib.contextmanager
def fixture():
    with tempfile.TemporaryDirectory(prefix='telemetry-modes-', dir='build') as tmp, receiver() as otlp:
        root = Path(tmp).resolve()
        home = root / 'home'
        config = home / '.config/capstan'
        config.mkdir(parents=True)
        (home / '.local/state').mkdir(parents=True)
        work = root / 'work'
        work.mkdir()
        (work / 'fixture.txt').write_text('FIXTURE_TOOL_PRIVATE\n')
        provider = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        provider.daemon_threads = True
        provider.requests = []
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        (config / 'config.lua').write_text('''return {
  provider = "fixture",
  providers = {fixture = {api_key = "fixture-not-a-secret", model = "fixture",
    endpoint = "http://127.0.0.1:%d/chat/completions", context_limit = 65536,
    models = {{id = "fixture", context_limit = 65536}}}},
  observability = {enabled = true, endpoint = "%s", service_name = "modes-test"},
}\n''' % (provider.server_port, otlp.base))
        env = {'HOME': str(home), 'PATH': os.defpath, 'TERM': 'xterm-256color',
               'LANG': 'en_US.UTF-8', 'NO_PROXY': '*', 'no_proxy': '*',
               'XDG_CONFIG_HOME': str(home / '.config'),
               'XDG_STATE_HOME': str(home / '.local/state'),
               'XDG_CACHE_HOME': str(home / '.cache'),
               'XDG_DATA_HOME': str(home / '.local/share'),
               'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null'}
        args = ['--no-mcp', '--no-wiki', '--yolo', '--provider', 'fixture',
                '--model', 'fixture', '--session-id', 'fixture-session',
                '--workdir', str(work), '--workspace', str(work), '--max-turns', '3']
        try:
            yield root, work, env, args, provider, otlp
        finally:
            provider.shutdown()
            provider.server_close()
            thread.join()


class ModeTests(unittest.TestCase):
    def inspect(self, provider, otlp):
        self.assertEqual(len(provider.requests), 2)
        self.assertTrue(any(m.get('role') == 'tool' and 'FIXTURE_TOOL_PRIVATE' in
                            str(m.get('content')) for m in provider.requests[1]['messages']))
        spans, logs = [], []
        for path, headers, body, _ in otlp.requests:
            self.assertIn(path, ['/v1/traces', '/v1/logs'])
            self.assertEqual(headers['Content-Type'], 'application/x-protobuf')
            for private in (PROMPT, ANSWER, 'FIXTURE_TOOL_PRIVATE', 'fixture-not-a-secret'):
                self.assertNotIn(private.encode(), body)
            records, resources = decode(body)
            self.assertTrue(all(r['service.name'] == 'modes-test' for r in resources))
            (spans if path.endswith('/traces') else logs).extend(records)
        self.assertEqual(collections.Counter(one(s, 5, 2) for s in spans),
                         {b'agent.run': 1, b'agent.model': 2, b'agent.tool': 1})
        root = next(s for s in spans if one(s, 5, 2) == b'agent.run')
        self.assertNotIn(4, root)
        ids = {one(s, 2, 2) for s in spans}
        self.assertEqual(len(ids), 4)
        for span in spans:
            self.assertEqual(one(span, 1, 2), one(root, 1, 2))
            if span is not root:
                self.assertEqual(one(span, 4, 2), one(root, 2, 2))
            self.assertEqual(attributes(span, 9)['outcome'], 'success')
        events = collections.Counter(one(wire(one(log, 5, 2)), 1, 2) for log in logs)
        self.assertEqual(events, {b'runtime.started': 1, b'span.started': 4, b'span.finished': 4})
        for request in provider.requests:
            encoded = json.dumps(request)
            for event in ('runtime.started', 'span.started', 'span.finished', 'modes-test'):
                self.assertNotIn(event, encoded)

    def test_cli_json_trace_and_otlp(self):
        with fixture() as (root, work, env, args, provider, otlp):
            trace = root / 'trace.jsonl'
            proc = subprocess.run([BINARY, 'run', *args, '--json', '--prompt', PROMPT,
                                   '--trace-file', str(trace)], cwd=work, env=env,
                                  capture_output=True, text=True, timeout=20)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            result = json.loads(proc.stdout)
            self.assertEqual(set(result), {'ok', 'text', 'error'})
            self.assertTrue(result['ok'])
            self.assertEqual(result['text'], ANSWER)
            events = [json.loads(line) for line in trace.read_text().splitlines()]
            self.assertEqual(events[0]['event'], 'run.started')
            self.assertEqual(events[-1]['event'], 'run.finished')
            self.assertTrue(events[-1]['data']['ok'])
            self.assertFalse(Path(str(trace) + '.partial').exists())
            self.assertEqual(trace.stat().st_mode & 0o777, 0o600)
            self.inspect(provider, otlp)

    def test_tui_prompt_idle_export(self):
        with fixture() as (root, work, env, args, provider, otlp):
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 110, 0, 0))
            proc = subprocess.Popen([BINARY, *args], cwd=work, env=env, stdin=slave,
                                    stdout=slave, stderr=slave, start_new_session=True)
            os.close(slave)
            screen = bytearray()

            def until(predicate):
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if select.select([master], [], [], .05)[0]:
                        try:
                            screen.extend(os.read(master, 65536))
                        except OSError:
                            pass
                    if predicate():
                        return
                    if proc.poll() is not None:
                        break
                logs = '\n'.join(p.read_text()[-4000:] for p in
                                 (root / 'home/.local/state/capstan/logs').rglob('*.jsonl'))
                self.fail('TUI timeout/early exit: ' + repr(bytes(screen[-2000:])) + '\n' + logs)

            try:
                until(lambda: b'ready' in screen)
                os.write(master, (PROMPT + '\r').encode())
                until(lambda: ANSWER.encode() in screen)
                # Match the existing PTY harness: terminate only after idle export.
                # This does not claim coverage of graceful TUI shutdown flushing.
                def exported():
                    logs = [record for path, _, body, _ in otlp.requests
                            if path.endswith('/logs') for record in decode(body)[0]]
                    return sum(one(wire(one(log, 5, 2)), 1, 2) == b'span.finished'
                               for log in logs) == 4 and any(
                                   path.endswith('/traces') for path, *_ in otlp.requests)
                until(exported)
                for event in (b'runtime.started', b'span.started', b'span.finished', b'modes-test'):
                    self.assertNotIn(event, screen)
                self.inspect(provider, otlp)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                os.close(master)


if __name__ == '__main__':
    unittest.main()
