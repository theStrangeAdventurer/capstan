#!/usr/bin/env python3
"""Real-binary CLI/TUI OTLP checks; only disposable workspace files and loopback.

Run: make build/capstan && python3 test/test_telemetry_modes.py [build/capstan]
No user configuration/environment is read.
Benchmark: python3 test/test_telemetry_modes.py build/capstan --benchmark
"""
import collections
import contextlib
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import queue
import hashlib
import platform
import statistics
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
def fixture(enabled=True, include_session_name=False):
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
  observability = {enabled = %s, endpoint = "%s", service_name = "modes-test",
    include_session_name = %s},
}\n''' % (provider.server_port, str(enabled).lower(), otlp.base,
          str(include_session_name).lower()))
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
    def inspect(self, provider, otlp, mode='cli', session_name=None):
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
        span_count = 7 if mode == 'cli' else (6 if mode == 'tui' else 5)
        self.assertEqual(collections.Counter(one(s, 5, 2) for s in spans),
                         {b'agent.run': 1, b'agent.model': 2, b'agent.tool': 1,
                          b'operation': span_count - 4})
        root = next(s for s in spans if one(s, 5, 2) == b'agent.run')
        tool = next(s for s in spans if one(s, 5, 2) == b'agent.tool')
        permission = next(s for s in spans
                          if attributes(s, 9).get('operation') == 'permission_decision')
        if mode == 'cli':
            startup = [s for s in spans if attributes(s, 9).get('operation') == 'startup']
            self.assertEqual(len(startup), 1)
            start = int.from_bytes(one(startup[0], 7, 1), 'little')
            end = int.from_bytes(one(startup[0], 8, 1), 'little')
            self.assertGreater(start, 0)
            self.assertGreaterEqual(end, start)
            self.assertEqual(end, int.from_bytes(one(root, 7, 1), 'little'))
            self.assertAlmostEqual(attributes(startup[0], 9)['duration_ms'],
                                   (end - start) / 1e6)
        if mode in ('cli', 'tui'):
            persistence = next(s for s in spans
                               if attributes(s, 9).get('operation') == 'session_save')
            self.assertGreaterEqual(int.from_bytes(one(persistence, 7, 1), 'little'),
                                    int.from_bytes(one(root, 8, 1), 'little'))
            self.assertGreaterEqual(attributes(persistence, 9)['duration_ms'], 0)
        self.assertEqual(attributes(permission, 9)['operation'], 'permission_decision')
        self.assertEqual(attributes(permission, 9)['purpose'], 'allow')
        self.assertEqual(attributes(permission, 9)['tool'], 'file_read')
        self.assertNotIn(4, root)
        ids = {one(s, 2, 2) for s in spans}
        self.assertEqual(len(ids), span_count)

        def context(record, field):
            attrs = attributes(record, field)
            self.assertEqual(attrs['mode'], mode)
            self.assertEqual(attrs['run.id'], one(root, 2, 2).hex())
            if mode == 'acp':
                self.assertNotIn('session.id', attrs)
            else:
                self.assertEqual(attrs['session.id'], 'fixture-session')
            if session_name is None:
                self.assertNotIn('session.name', attrs)
            else:
                self.assertEqual(attrs['session.name'], session_name)

        for span in spans:
            context(span, 9)
            self.assertEqual(one(span, 1, 2), one(root, 1, 2))
            if span is not root:
                parent = tool if span is permission else root
                self.assertEqual(one(span, 4, 2), one(parent, 2, 2))
            self.assertEqual(attributes(span, 9)['outcome'], 'success')
        events = collections.Counter(one(wire(one(log, 5, 2)), 1, 2) for log in logs)
        expected = {b'span.started': span_count, b'span.finished': span_count}
        if mode != 'cli':
            expected[b'runtime.started'] = 1
        self.assertEqual(events, expected)
        lifecycle = collections.Counter()
        for log in logs:
            event = one(wire(one(log, 5, 2)), 1, 2)
            if event == b'runtime.started':
                self.assertNotIn(9, log)
                self.assertNotIn(10, log)
                self.assertEqual(attributes(log, 6), {})
                continue
            context(log, 6)
            self.assertEqual(one(log, 9, 2), one(root, 1, 2))
            sid = one(log, 10, 2)
            self.assertIn(sid, ids)
            lifecycle[sid, event] += 1
            span = next(s for s in spans if one(s, 2, 2) == sid)
            self.assertEqual(attributes(log, 6)['span.name'], one(span, 5, 2).decode())
        self.assertEqual(lifecycle, collections.Counter({(sid, event): 1 for sid in ids
                         for event in (b'span.started', b'span.finished')}))
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
            contexts = [e['data'] for e in events if e['event'] == 'run.context']
            self.assertEqual(len(contexts), 1)
            spans = [s for path, _, body, _ in otlp.requests
                     if path.endswith('/traces') for s in decode(body)[0]]
            native_root = next(s for s in spans if one(s, 5, 2) == b'agent.run')
            self.assertEqual(contexts[0], {
                'trace_id': one(native_root, 1, 2).hex(),
                'span_id': one(native_root, 2, 2).hex(),
                'run_id': attributes(native_root, 9)['run.id'],
                'session_id': attributes(native_root, 9)['session.id'],
            })
            persistence = next(s for s in spans
                               if attributes(s, 9).get('operation') == 'session_save')
            self.assertEqual(attributes(persistence, 9)['duration_ms'],
                             events[-1]['data']['timing']['session_save_ms'])
            # Exercise the real benchmark consumer against the real CLI export.
            import importlib.util
            spec = importlib.util.spec_from_file_location('trace_consumer',
                Path(__file__).resolve().parents[1] /
                'benchmarks/polyglot/scripts/run_eval.py')
            consumer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(consumer)
            summary = consumer.load_trace_summary(trace)
            self.assertEqual(summary['status'], 'complete')
            self.assertEqual(summary['correlation'], {
                'legacy_run_id': events[0]['run_id'],
                **{key: contexts[0][key] for key in ('run_id', 'trace_id', 'span_id', 'session_id')},
            })

    def test_cli_trace_free_canonical_log_summary(self):
        # Import the actual harness and analyzer, without running an evaluation.
        import importlib
        scripts = Path(__file__).resolve().parents[1] / 'benchmarks/polyglot/scripts'
        sys.path.insert(0, str(scripts))
        try:
            consumer = importlib.import_module('run_eval')
            analyzer = importlib.import_module('analyze_traces')
        finally:
            sys.path.pop(0)

        with fixture() as (root, work, env, args, provider, otlp):
            started = time.monotonic()
            proc = subprocess.run([BINARY, 'run', *args, '--json', '--prompt', PROMPT],
                                  cwd=work, env=env, capture_output=True, text=True, timeout=20)
            seconds = time.monotonic() - started
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(json.loads(proc.stdout), {'ok': True, 'text': ANSWER, 'error': ''})
            self.inspect(provider, otlp)
            spans = [s for path, _, body, _ in otlp.requests
                     if path.endswith('/traces') for s in decode(body)[0]]
            native_root = next(s for s in spans if one(s, 5, 2) == b'agent.run')
            attrs = attributes(native_root, 9)
            correlation = {
                'trace_id': one(native_root, 1, 2).hex(),
                'span_id': one(native_root, 2, 2).hex(),
                'run_id': attrs['run.id'],
                'session_id': 'fixture-session',
            }
            # Select the untouched native session log, not a synthesized trace.
            log_dir = root / 'home/.local/state/capstan/logs'
            candidates = []
            for path in log_dir.rglob('*.jsonl'):
                records = [json.loads(line) for line in path.read_text().splitlines()]
                self.assertTrue(all(r['schema'] == 'capstan.log.v1' for r in records))
                if any(r.get('category') == 'telemetry' and
                       r.get('message') == 'span.finished' and
                       r.get('span_id') == correlation['span_id'] for r in records):
                    candidates.append(path)
            self.assertEqual(len(candidates), 1)
            log_path = candidates[0]
            summary = consumer.load_log_summary(log_path, session_id='fixture-session')
            self.assertEqual(summary['status'], 'complete', summary)
            self.assertTrue(summary['complete'])
            self.assertFalse(summary['partial'])
            self.assertEqual(summary['schema'], 'capstan.log.v1')
            self.assertEqual(summary['source'], str(log_path))
            self.assertEqual(summary['correlation'], correlation)
            self.assertEqual(summary['observed_events'], 2 * len(spans))
            self.assertEqual(summary['terminal'], {
                'ok': True, 'outcome': attrs['outcome'],
                'duration_ms': attrs['duration_ms'], 'turns': attrs['turns'],
                'counts': {'model_requests': attrs['request_count'],
                           'tool_calls': attrs['tool_count']},
                'breakdown': {key: attrs[key] for key in (
                    'model_ms', 'tool_ms', 'permission_wait_ms', 'subagent_wait_ms',
                    'unattributed_ms', 'overlap_ms')},
            })
            self.assertEqual(summary['terminal']['turns'], 2)
            self.assertGreater(summary['terminal']['duration_ms'], 0)
            for selector in ({'trace_id': correlation['trace_id']},
                             {'run_id': correlation['run_id']}):
                self.assertEqual(consumer.load_log_summary(log_path, **selector), summary)
            self.assertEqual(consumer.load_log_summary(
                log_path, session_id='other-session')['status'], 'corrupt')
            reconciled = consumer.reconcile_process_telemetry(
                summary, timed_out=False, return_code=proc.returncode)
            self.assertEqual(reconciled, summary)
            row = {'status': 'passed', 'seconds': seconds, 'agent': {
                'seconds': seconds, 'return_code': proc.returncode,
                'timed_out': False, 'telemetry': reconciled}}
            analyzer.validate_result_row(row, log_path)
            self.assertEqual(row['agent']['telemetry'], summary)
            self.assertEqual(analyzer.complete_telemetry_rows([row]), [row])
            self.assertEqual(analyzer.metric_telemetry_rows([row]), [row])
            self.assertEqual(summary['terminal']['counts'], {'model_requests': 2, 'tool_calls': 1})
            for metric in analyzer.DISPLAY_METRICS:
                self.assertEqual(analyzer.avg_telemetry([row], metric),
                                 summary['terminal'][metric[0]][metric[1]])
            template = analyzer.trace_url_template(
                'http://localhost/trace/{trace_id}?span={span_id}')
            self.assertEqual(analyzer.backend_trace_links([row], template), [{
                'replicate_id': None, 'status': 'complete',
                'url': template.format(**correlation),
            }])

    def test_cli_session_name_opt_in(self):
        with fixture(include_session_name=True) as (_, work, env, args, provider, otlp):
            proc = subprocess.run([BINARY, 'run', *args, '--json', '--prompt', PROMPT],
                                  cwd=work, env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(json.loads(proc.stdout)['text'], ANSWER)
            self.inspect(provider, otlp, session_name='fixture-session')

    def test_acp_ephemeral_context(self):
        # Even opt-in names must not turn ACP's ephemeral ID into durable context.
        with fixture(include_session_name=True) as (_, work, env, args, provider, otlp):
            proc = subprocess.Popen([BINARY, 'acp', '--yolo'], cwd=work, env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            messages = queue.Queue()
            reader = threading.Thread(target=lambda: [messages.put(line) for line in proc.stdout],
                                      daemon=True)
            reader.start()
            seen = []

            def request(number, method, params):
                proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': number,
                                             'method': method, 'params': params}) + '\n')
                proc.stdin.flush()
                deadline = time.monotonic() + 15
                while True:
                    try:
                        message = json.loads(messages.get(timeout=max(.01, deadline - time.monotonic())))
                    except queue.Empty:
                        self.fail('ACP response timeout: ' + repr(seen))
                    seen.append(message)
                    if message.get('id') == number:
                        self.assertNotIn('error', message)
                        return message['result']

            try:
                self.assertEqual(request(1, 'initialize', {'protocolVersion': 1,
                                 'clientCapabilities': {}})['protocolVersion'], 1)
                session = request(2, 'session/new', {'cwd': str(work), 'mcpServers': []})
                self.assertTrue(session['sessionId'].startswith('capstan-'))
                result = request(3, 'session/prompt', {'sessionId': session['sessionId'],
                                 'prompt': [{'type': 'text', 'text': PROMPT}]})
                self.assertEqual(result['stopReason'], 'end_turn')
                self.assertIn(ANSWER, json.dumps(seen))
                proc.stdin.close()
                self.assertEqual(proc.wait(timeout=10), 0)
                self.inspect(provider, otlp, mode='acp')
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                reader.join(timeout=2)
                for stream in (proc.stdin, proc.stdout, proc.stderr):
                    stream.close()

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
                               for log in logs) == 6 and any(
                                   path.endswith('/traces') for path, *_ in otlp.requests)
                until(exported)
                for event in (b'runtime.started', b'span.started', b'span.finished', b'modes-test'):
                    self.assertNotIn(event, screen)
                self.inspect(provider, otlp, mode='tui')
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                os.close(master)


def measured_process(command, *, cwd, env, timeout):
    """Reap exactly this PID with wait4; never use cumulative child peak RSS.

    Files avoid pipe backpressure without communicate()/poll() reaping the child.
    The 1ms polling interval is included in wall time for both fixture variants.
    """
    if not hasattr(os, 'wait4') or sys.platform not in ('darwin', 'linux'):
        raise RuntimeError('RSS benchmark requires wait4 on macOS or Linux')
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        start = time.perf_counter_ns()
        proc = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                stdout=stdout, stderr=stderr)
        deadline = time.monotonic() + timeout
        try:
            while True:
                pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
                if pid:
                    proc.returncode = os.waitstatus_to_exitcode(status)
                    break
                if time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(command, timeout)
                time.sleep(.001)
        finally:
            if proc.returncode is None:
                os.kill(proc.pid, 9)
                _, status, _ = os.wait4(proc.pid, 0)
                proc.returncode = os.waitstatus_to_exitcode(status)
        wall_ms = (time.perf_counter_ns() - start) / 1e6
        stdout.seek(0)
        stderr.seek(0)
        result = subprocess.CompletedProcess(command, proc.returncode,
                                             stdout.read().decode(), stderr.read().decode())
    return result, {'wall_ms': wall_ms, 'child_user_ms': usage.ru_utime * 1000,
                    'child_system_ms': usage.ru_stime * 1000,
                    'peak_rss_bytes': usage.ru_maxrss * (1 if sys.platform == 'darwin' else 1024)}


class MeasurementTests(unittest.TestCase):
    def test_per_process_usage_and_output(self):
        proc, measured = measured_process(
            [sys.executable, '-I', '-c', 'print("fixture"); raise SystemExit(7)'],
            cwd=Path.cwd(), env={'PATH': os.defpath}, timeout=5)
        self.assertEqual((proc.returncode, proc.stdout, proc.stderr), (7, 'fixture\n', ''))
        self.assertGreater(measured['peak_rss_bytes'], 0)
        self.assertGreater(measured['wall_ms'], 0)
        self.assertGreaterEqual(measured['child_user_ms'], 0)
        self.assertGreaterEqual(measured['child_system_ms'], 0)

    def test_timeout_reaps_child(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            measured_process([sys.executable, '-I', '-c', 'import time; time.sleep(10)'],
                             cwd=Path.cwd(), env={'PATH': os.defpath}, timeout=.02)


def benchmark():
    """Fresh CLI process/session each time; setup/teardown excluded from timing."""
    samples = []
    checks = ModeTests()
    for pair in range(11):
        # One warm-up pair; reverse ordering to reduce systematic drift.
        for enabled in ((False, True) if pair % 2 == 0 else (True, False)):
            with fixture(enabled=enabled) as (_, work, env, args, provider, otlp):
                proc, measured = measured_process(
                    [BINARY, 'run', *args, '--json', '--prompt', PROMPT],
                    cwd=work, env=env, timeout=20)
                checks.assertGreater(measured['peak_rss_bytes'], 0)
                checks.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                checks.assertEqual(json.loads(proc.stdout), {'ok': True, 'text': ANSWER, 'error': ''})
                checks.assertEqual(len(provider.requests), 2)
                if enabled:
                    checks.inspect(provider, otlp)
                else:
                    checks.assertEqual(otlp.requests, [])
                samples.append({'pair': pair, 'warmup': pair == 0, 'enabled': enabled,
                                **measured,
                                'otlp_requests': len(otlp.requests),
                                'otlp_bytes': sum(len(r[2]) for r in otlp.requests)})
    medians = {label: statistics.median(s['wall_ms'] for s in samples
               if not s['warmup'] and s['enabled'] == enabled)
               for label, enabled in (('off_ms', False), ('on_ms', True))}
    medians['delta_ms'] = medians['on_ms'] - medians['off_ms']
    medians['delta_percent'] = 100 * medians['delta_ms'] / medians['off_ms']
    report = {'command': 'python3 test/test_telemetry_modes.py build/capstan --benchmark',
              'platform': platform.platform(), 'python': platform.python_version(),
              'binary_sha256': hashlib.sha256(Path(BINARY).read_bytes()).hexdigest(),
              'method': '10 alternating off/on pairs after one warmup pair; fresh isolated CLI sessions; '
                        'two loopback SSE requests and one file_read; fixture setup/teardown excluded',
              'limitations': 'Wall time includes startup, local logs and shutdown export. '
                             'Loopback collector, no real model/network spend; not isolated instrumentation '
                             'cost or production/TUI/ACP latency. Shared-host scheduling noise; '
                             'wall includes up to 1ms wait4 polling delay. RSS is whole-process peak, '
                             'not allocations attributable solely to telemetry.',
              'resource_method': 'Per-PID os.wait4 rusage; macOS ru_maxrss bytes, Linux KiB converted to bytes; '
                                 'CPU user/system seconds converted to milliseconds; no cumulative child deltas',
              'rss_medians_bytes': {label: statistics.median(s['peak_rss_bytes'] for s in samples
                                    if not s['warmup'] and s['enabled'] == enabled)
                                    for label, enabled in (('off', False), ('on', True))},
              'wall_medians': medians, 'samples': samples}
    output = Path('build/telemetry-modes-overhead.json')
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(str(output) + ': ' + json.dumps(medians))


if __name__ == '__main__':
    if sys.argv[1:] == ['--benchmark']:
        benchmark()
    else:
        unittest.main()
