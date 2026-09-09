#!/usr/bin/env python3
"""Dependency-free OTLP integration tests. Build driver via the parent Makefile.
Run: python3 test/test_telemetry_otlp.py build/test_telemetry_native
Only loopback networking; no config files or credential-bearing environment used.
"""
import collections
import contextlib
import http.server
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

DRIVER = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith('-')
                  else 'build/test_telemetry_native').resolve())


def wire(data):
    """Generic strict protobuf reader, independent of the native encoder."""
    pos = 0
    fields = collections.defaultdict(list)

    def varint():
        nonlocal pos
        value = 0
        for shift in range(0, 70, 7):
            if pos >= len(data):
                raise ValueError('truncated varint')
            byte = data[pos]
            pos += 1
            if shift == 63 and byte > 1:
                raise ValueError('varint overflow')
            value |= (byte & 127) << shift
            if not byte & 128:
                return value
        raise ValueError('unterminated varint')

    while pos < len(data):
        tag = varint()
        field, kind = tag >> 3, tag & 7
        if not field:
            raise ValueError('zero field')
        if kind == 0:
            value = varint()
        else:
            size = varint() if kind == 2 else {1: 8, 5: 4}.get(kind)
            if size is None or size > len(data) - pos:
                raise ValueError('invalid wire type or truncated field')
            value = data[pos:pos + size]
            pos += size
        fields[field].append((kind, value))
    return fields


def one(fields, number, kind):
    values = fields[number]
    assert len(values) == 1, (number, 'field cardinality', len(values))
    actual_kind, value = values[0]
    assert actual_kind == kind, (number, actual_kind, kind)
    return value


def attributes(fields, number):
    result = {}
    for kind, raw in fields.get(number, []):
        assert kind == 2
        kv = wire(raw)
        key = one(kv, 1, 2).decode('utf-8')
        assert key not in result, ('duplicate attribute', key)
        av = wire(one(kv, 2, 2))
        assert len(av) == 1
        if 1 in av:
            value = one(av, 1, 2).decode('utf-8')
        elif 2 in av:
            value = bool(one(av, 2, 0))
        elif 3 in av:
            value = one(av, 3, 0)
        else:
            value = struct.unpack('<d', one(av, 4, 1))[0]
        result[key] = value
    return result


def decode(body):
    request = wire(body)
    records, resources = [], []
    for kind, raw in request[1]:
        assert kind == 2
        resource_signal = wire(raw)
        resources.append(attributes(wire(one(resource_signal, 1, 2)), 1))
        for kind, raw_scope in resource_signal[2]:
            assert kind == 2
            scope_signal = wire(raw_scope)
            scope = wire(one(scope_signal, 1, 2))
            assert one(scope, 1, 2) == b'capstan.native'
            assert one(scope, 2, 2) == b'1'
            for kind, record in scope_signal[2]:
                assert kind == 2
                records.append(wire(record))
    assert records and resources
    return records, resources


class Receiver(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, policy):
        super().__init__(('127.0.0.1', 0), Handler)
        self.policy = policy
        self.requests = []
        self.lock = threading.Lock()
        self.base = 'http://127.0.0.1:%d' % self.server_port


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        with self.server.lock:
            self.server.requests.append((self.path, dict(self.headers), body, time.monotonic()))
            index = len(self.server.requests)
        status, headers, response, delay = self.server.policy(index)
        time.sleep(delay)
        try:
            self.send_response(status)
            self.send_header('Content-Length', str(len(response)))
            for key, value in headers.items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(response)
        except (BrokenPipeError, ConnectionResetError):
            pass


@contextlib.contextmanager
def receiver(policy=lambda _: (200, {}, b'', 0)):
    server = Receiver(policy)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def run(mode, base, env=None):
    # Whitelist, not merely removal of OTEL_*: no inherited proxies, headers,
    # Lua startup variables, or runtime configuration can influence the driver.
    clean = {'PATH': os.defpath, 'NO_PROXY': '*', 'no_proxy': '*', 'LC_ALL': 'C'}
    clean.update(env or {})
    start = time.monotonic()
    proc = subprocess.run([DRIVER, mode, base], env=clean, capture_output=True,
                          text=True, timeout=12)
    assert proc.returncode == 0, 'native driver failed: ' + proc.stderr
    assert 'FORBIDDEN_RAW' not in proc.stdout + proc.stderr
    assert 'FAKE_TEST_VALUE' not in proc.stdout + proc.stderr
    assert 'COLLECTOR_PRIVATE' not in proc.stdout + proc.stderr
    return proc.stdout, time.monotonic() - start


class OTLPTests(unittest.TestCase):
    def test_tool_details_opt_in_and_common_redaction(self):
        for mode in ('details', 'details-off'):
            with self.subTest(mode=mode), receiver() as server:
                run(mode, server.base)
                spans, logs = self.inspect(server)
            child, = [s for s in spans if one(s, 5, 2) == b'subagent']
            records = [(child, 9)] + [(log, 6) for log in logs
                       if attributes(log, 6).get('span.name') == 'subagent']
            for record, field in records:
                attrs = attributes(record, field)
                self.assertEqual(attrs['subagent_index'], 2)
                self.assertEqual(attrs['subagent_id'], 'api_key=[REDACTED]')
                self.assertEqual(attrs['attempt'], 1)
                if mode == 'details-off':
                    self.assertNotIn('shell.command', attrs)
                    self.assertNotIn('tool.target', attrs)
                else:
                    # Preserve the canonical redactor's existing whitespace normalization.
                    self.assertEqual(attrs['shell.command'], 'echoapi_key=[REDACTED]')
                    self.assertEqual(attrs['tool.target'], 'fixture.lua')
            bounded, = [s for s in spans if one(s, 5, 2) == b'tool']
            records = [(bounded, 9)] + [(log, 6) for log in logs
                       if attributes(log, 6).get('span.name') == 'tool'
                       and attributes(log, 6).get('outcome')]
            for record, field in records:
                attrs = attributes(record, field)
                if mode == 'details':
                    self.assertEqual(attrs['shell.command'], '[OVERSIZED]')
                    self.assertEqual(attrs['tool.target'], 'api_key=[REDACTED]')
                else:
                    self.assertNotIn('shell.command', attrs)
                    self.assertNotIn('tool.target', attrs)
        with receiver() as server:
            stdout, _ = run('details-invalid', server.base)
            self.assertIn('disabled', stdout)
            self.assertEqual(server.requests, [])

    def test_exporter_result_classification(self):
        # Pure C policy: no Lua state, collector, DNS, TLS backend or timing.
        stdout, _ = run('classification', '')
        self.assertEqual(stdout, 'classification ok\n')

    def inspect(self, server, prefix='', header='config', service='native-test', suite='native'):
        spans, logs = [], []
        for path, headers, body, _ in server.requests:
            self.assertIn(path, [prefix + '/v1/traces', prefix + '/v1/logs'])
            headers = {k.lower(): v for k, v in headers.items()}
            self.assertEqual(headers['content-type'], 'application/x-protobuf')
            self.assertEqual(headers['x-test'], header)
            self.assertEqual(int(headers['content-length']), len(body))
            self.assertNotIn(b'FORBIDDEN_RAW', body)
            self.assertNotIn(b'FAKE_TEST_VALUE', body)
            records, resources = decode(body)
            for resource in resources:
                self.assertEqual(resource, {'service.name': service,
                                           'service.version': 'test-1', 'suite': suite})
            (spans if path.endswith('/traces') else logs).extend(records)
        return spans, logs

    def test_sample_ids_attributes_lifecycle_and_repeated_end(self):
        with receiver() as server:
            stdout, _ = run('sample', server.base)
            self.assertEqual(stdout, 'enabled\n')
            spans, logs = self.inspect(server)
        self.assertEqual(len(spans), 3)
        self.assertEqual(len(logs), 6)
        by_name = {one(s, 5, 2): s for s in spans}
        root, model, tool = [by_name[n] for n in (b'agent.run', b'agent.model', b'agent.tool')]
        trace = one(root, 1, 2)
        self.assertEqual(len(trace), 16)
        self.assertNotEqual(trace, bytes(16))
        self.assertNotIn(4, root)
        self.assertEqual(one(model, 4, 2), one(root, 2, 2))
        self.assertEqual(one(tool, 4, 2), one(model, 2, 2))
        ids = set()
        for span in spans:
            self.assertEqual(one(span, 1, 2), trace)
            sid = one(span, 2, 2)
            self.assertEqual(len(sid), 8)
            self.assertNotEqual(sid, bytes(8))
            ids.add(sid)
            start = int.from_bytes(one(span, 7, 1), 'little')
            end = int.from_bytes(one(span, 8, 1), 'little')
            self.assertGreater(start, 0)
            self.assertGreaterEqual(end, start)
            self.assertEqual(one(span, 6, 0), 1)
        self.assertEqual(len(ids), 3)
        context = {'session.id': 'native-session', 'run.id': one(root, 2, 2).hex()}
        self.assertEqual(attributes(root, 9), dict(context, operation='run', depth=0,
                         provider='api_key=[REDACTED]', model='fixture-model',
                         cancelled=False, outcome='success'))
        self.assertEqual(attributes(model, 9), dict(context, input_tokens=12, profile='fixture',
                         output_tokens=3, cancelled=False, outcome='error'))
        self.assertEqual(attributes(tool, 9), dict(context, tool='shell', duration_ms=2.5,
                         cancelled=True, outcome='cancelled'))
        for span, status in [(root, 1), (model, 2), (tool, 0)]:
            self.assertEqual(one(wire(one(span, 15, 2)), 3, 0), status)
        events = collections.Counter()
        for log in logs:
            self.assertEqual(one(log, 9, 2), trace)
            sid = one(log, 10, 2)
            self.assertIn(sid, ids)
            event = one(wire(one(log, 5, 2)), 1, 2)
            events[sid, event] += 1
            error = sid == one(model, 2, 2) and event == b'span.finished'
            self.assertEqual(one(log, 2, 0), 17 if error else 9)
            self.assertEqual(one(log, 3, 2), b'ERROR' if error else b'INFO')
            self.assertGreater(int.from_bytes(one(log, 1, 1), 'little'), 0)
            span = next(s for s in spans if one(s, 2, 2) == sid)
            expected = attributes(span, 9)
            if event == b'span.started':
                for key in ('outcome', 'cancelled', 'duration_ms', 'output_tokens'):
                    expected.pop(key, None)
            expected['span.name'] = one(span, 5, 2).decode()
            if 4 in span:
                expected['parent.span_id'] = one(span, 4, 2).hex()
            self.assertEqual(attributes(log, 6), expected)
        self.assertEqual(events, collections.Counter({(sid, event): 1 for sid in ids
                         for event in (b'span.started', b'span.finished')}))

    def test_closed_error_categories(self):
        with receiver() as server:
            run('categories', server.base)
            spans, logs = self.inspect(server)
        self.assertEqual((len(spans), len(logs)), (6, 12))
        categorized = [s for s in spans if 'error.category' in attributes(s, 9)]
        self.assertEqual(len(categorized), 1)
        self.assertEqual(attributes(categorized[0], 9)['error.category'], 'transport')
        categorized_logs = [log for log in logs if 'error.category' in attributes(log, 6)]
        self.assertEqual(len(categorized_logs), 1)
        self.assertEqual(attributes(categorized_logs[0], 6)['error.category'], 'transport')
        self.assertEqual(one(categorized_logs[0], 10, 2), one(categorized[0], 2, 2))
        self.assertEqual(one(wire(one(categorized_logs[0], 5, 2)), 1, 2), b'span.finished')

    def test_network_phases_on_spans_and_correlated_logs(self):
        with receiver() as server:
            run('phases', server.base)
            spans, logs = self.inspect(server)
        self.assertEqual((len(spans), len(logs)), (1, 2))
        expected = {'transport.' + key: i / 2 for i, key in enumerate([
            'namelookup_elapsed_ms', 'connect_elapsed_ms', 'appconnect_elapsed_ms',
            'pretransfer_elapsed_ms', 'starttransfer_elapsed_ms', 'total_ms',
            'dns_ms', 'tcp_connect_ms', 'tls_handshake_ms', 'request_setup_ms',
            'upload_and_server_wait_ms', 'download_ms'], 1)}
        expected.update({'transport.upload_bytes': 123, 'transport.download_bytes': 456})
        actual = attributes(spans[0], 9)
        self.assertEqual({k: v for k, v in actual.items() if k.startswith('transport.')}, expected)
        finished, = [log for log in logs
                     if one(wire(one(log, 5, 2)), 1, 2) == b'span.finished']
        self.assertEqual(attributes(finished, 6), dict(actual, **{'span.name': 'agent.run'}))

    def test_startup_once_uncorrelated_and_disabled_discarded(self):
        with receiver() as server:
            stdout, _ = run('startup', server.base)
            self.assertEqual(stdout, 'enabled\n')
            spans, logs = self.inspect(server)
        self.assertEqual((len(spans), len(logs)), (3, 7))
        events = collections.Counter(one(wire(one(log, 5, 2)), 1, 2) for log in logs)
        self.assertEqual(events, {b'runtime.started': 1, b'span.started': 3,
                                  b'span.finished': 3})
        startup, = [log for log in logs
                    if one(wire(one(log, 5, 2)), 1, 2) == b'runtime.started']
        self.assertEqual(set(startup), {1, 2, 3, 5})
        self.assertEqual(one(startup, 2, 0), 9)
        self.assertEqual(one(startup, 3, 2), b'INFO')
        timestamp = int.from_bytes(one(startup, 1, 1), 'little')
        self.assertGreater(timestamp, 0)
        self.assertLessEqual(timestamp, min(int.from_bytes(one(s, 7, 1), 'little')
                                            for s in spans))
        with receiver() as server:
            stdout, _ = run('startup-disabled', server.base)
            self.assertEqual(stdout, 'disabled\n')
            self.assertEqual(server.requests, [])

    def test_distinct_roots_and_ephemeral_context(self):
        with receiver() as server:
            stdout, _ = run('roots', server.base)
            self.assertEqual(stdout, 'enabled\n')
            spans, logs = self.inspect(server)
        self.assertEqual((len(spans), len(logs)), (4, 8))
        roots = [s for s in spans if one(s, 5, 2) != b'title']
        self.assertEqual(len({attributes(s, 9)['run.id'] for s in roots}), 3)
        ephemeral, = [s for s in roots if attributes(s, 9).get('mode') == 'acp']
        self.assertNotIn('session.id', attributes(ephemeral, 9))
        self.assertNotIn('session.name', attributes(ephemeral, 9))
        root, = [s for s in roots if one(s, 5, 2) == b'agent.run']
        title, = [s for s in spans if one(s, 5, 2) == b'title']
        self.assertEqual(attributes(title, 9)['run.id'], attributes(root, 9)['run.id'])
        self.assertEqual(one(title, 4, 2), one(root, 2, 2))

    def test_session_context_immutable_after_completion(self):
        with receiver() as server:
            stdout, _ = run('context', server.base)
            self.assertEqual(stdout, 'enabled\n')
            spans, logs = self.inspect(server)
            for _, _, body, _ in server.requests:
                for value in (b'session_id', b'mutated-session',
                              b'completed-session'):
                    self.assertNotIn(value, body)
        self.assertEqual((len(spans), len(logs)), (3, 6))
        root, = [s for s in spans if one(s, 5, 2) == b'agent.run']
        for record, field in [(s, 9) for s in spans] + [(log, 6) for log in logs]:
            actual = attributes(record, field)
            self.assertEqual(actual['session.id'], 'native-session')
            self.assertEqual(actual['session.name'], 'api_key=[REDACTED]')
            self.assertEqual(actual['mode'], 'tui')
            self.assertEqual(actual['run.id'], one(root, 2, 2).hex())
        for span in spans:
            self.assertEqual(one(span, 1, 2), one(root, 1, 2))
            if span is not root:
                self.assertEqual(one(span, 4, 2), one(root, 2, 2))

    def test_config_specific_routing_and_headers(self):
        with receiver() as server:
            run('specific', server.base)
            self.assertEqual({r[0] for r in server.requests}, {'/custom/traces', '/custom/logs'})
            for path, headers, body, _ in server.requests:
                self.assertEqual(headers['X-Test'], 'trace-config' if path.endswith('traces') else 'log-config')
                decode(body)

    def test_environment_precedence(self):
        with receiver() as server:
            run('precedence', server.base + '/wrong', {
                'OTEL_EXPORTER_OTLP_ENDPOINT': server.base + '/env/',
                'OTEL_EXPORTER_OTLP_HEADERS': 'X-Test=env%20value',
                'OTEL_SERVICE_NAME': 'env-service',
                'OTEL_RESOURCE_ATTRIBUTES': 'suite=env,service.name=less-specific'})
            spans, logs = self.inspect(server, '/env', 'env value', 'env-service', 'env')
            self.assertEqual((len(spans), len(logs)), (3, 6))

    def test_signal_environment_and_empty_environment(self):
        with receiver() as server:
            run('sample', server.base, {'OTEL_EXPORTER_OTLP_ENDPOINT': '',
                'OTEL_EXPORTER_OTLP_TRACES_ENDPOINT': server.base + '/exact?test=1',
                'OTEL_EXPORTER_OTLP_TRACES_HEADERS': 'X-Test=specific',
                'OTEL_EXPORTER_OTLP_HEADERS': 'X-Test=generic'})
            self.assertEqual({r[0] for r in server.requests}, {'/exact?test=1', '/v1/logs'})
            for path, headers, body, _ in server.requests:
                self.assertEqual(headers['X-Test'], 'specific' if path.startswith('/exact') else 'generic')
                decode(body)

    def test_base_query_routing(self):
        with receiver() as server:
            run('sample', server.base + '/prefix/?test=1')
            self.assertEqual({r[0] for r in server.requests},
                             {'/prefix/v1/traces?test=1', '/prefix/v1/logs?test=1'})

    def test_disabled_isolated_invalid_and_signal_selection(self):
        cases = [('disabled', {}), ('isolated', {}), ('sample', {'OTEL_SDK_DISABLED': 'TrUe'}),
                 ('sample', {'OTEL_EXPORTER_OTLP_PROTOCOL': 'grpc'}),
                 ('sample', {'OTEL_EXPORTER_OTLP_HEADERS': 'Host=forbidden'}),
                 ('sample', {'OTEL_EXPORTER_OTLP_HEADERS': 'X-Test=bad%0D%0Ainjection'}),
                 ('sample', {'OTEL_TRACES_EXPORTER': 'none', 'OTEL_LOGS_EXPORTER': 'none'})]
        with receiver() as server:
            for mode, env in cases:
                with self.subTest(mode=mode, env=env):
                    stdout, _ = run(mode, server.base, env)
                    self.assertIn('disabled\n', stdout)
                    self.assertEqual(server.requests, [])
            run('single', server.base)
            self.assertEqual([r[0] for r in server.requests], ['/v1/traces'])

    def test_independent_file_and_network_sinks(self):
        # Driver asserts offline IDs, successful/repeated completion and local
        # lifecycle counts. Receiver independently proves network suppression.
        with receiver() as server:
            stdout, _ = run('offline-silent', server.base)
            self.assertEqual(stdout, 'disabled\n')
            self.assertEqual(server.requests, [])
            stdout, _ = run('network-only', server.base)
            self.assertEqual(stdout, 'enabled\n')
            spans, logs = self.inspect(server)
            self.assertEqual((len(spans), len(logs)), (3, 6))

    def test_no_redirects(self):
        with receiver() as destination:
            with receiver(lambda _: (307, {'Location': destination.base + '/stolen'}, b'', 0)) as server:
                run('single', server.base)
                self.assertEqual(len(server.requests), 1)
                self.assertEqual(destination.requests, [])

    def test_retry_after_and_identical_payload(self):
        with receiver(lambda i: (429, {'Retry-After': '2'}, b'', 0) if i == 1
                      else (200, {}, b'', 0)) as server:
            run('retry', server.base)
            self.assertEqual(len(server.requests), 2)
            first, second = server.requests
            self.assertGreaterEqual(second[3] - first[3], 1.95)
            self.assertEqual(first[:3], second[:3])

    def test_retry_limit(self):
        with receiver(lambda _: (503, {}, b'', 0)) as server:
            stdout, _ = run('retry', server.base)
            self.assertEqual(len(server.requests), 3)
            self.assertIn('losses 0 0 1 0', stdout)
            for a, b, minimum in [(0, 1, .95), (1, 2, 1.95)]:
                self.assertGreaterEqual(server.requests[b][3] - server.requests[a][3], minimum)
                self.assertEqual(server.requests[a][2], server.requests[b][2])

    def test_partial_success_and_malformed_are_not_retried(self):
        # ExportResponse.partial_success { rejected_*: 1, error_message: ... }
        message = b'COLLECTOR_PRIVATE'
        partial = b'\x08\x01\x12' + bytes([len(message)]) + message
        for response, diagnostic in [(b'\x0a' + bytes([len(partial)]) + partial, 'losses 0 1 0 0'),
                                     (b'\x0a\x80', 'losses 0 0 0 1')]:
            with self.subTest(response=response):
                with receiver(lambda _: (200, {}, response, 0)) as server:
                    stdout, _ = run('retry', server.base)
                    self.assertEqual(len(server.requests), 1)
                    self.assertIn(diagnostic, stdout)

    def test_cleanup_exports_root_before_blocked_lifecycle_logs(self):
        for failure in [(503, {'Retry-After': '60'}, b'', 0),
                        (200, {}, b'', 4)]:
            with self.subTest(failure=failure), receiver() as server:
                server.policy = lambda _: (failure if server.requests[-1][0].endswith('/logs')
                                           else (200, {}, b'', 0))
                stdout, elapsed = run('sample', server.base)
                spans, _ = self.inspect(server)
                self.assertEqual(server.requests[0][0], '/v1/traces')
                self.assertEqual(len(spans), 3)
                root, = [s for s in spans if one(s, 5, 2) == b'agent.run']
                self.assertNotIn(4, root)
                self.assertEqual(attributes(root, 9)['outcome'], 'success')
                self.assertIn('losses 6 0 0 0', stdout)
                self.assertLess(elapsed, 3.0)

    def test_cleanup_deadline_retry_after_and_slow_receiver(self):
        for policy in [lambda _: (503, {'Retry-After': '60'}, b'', 0),
                       lambda _: (200, {}, b'', 4)]:
            with receiver(policy) as server:
                _, elapsed = run('single', server.base)
                self.assertLess(elapsed, 3.0, 'cleanup exceeded its two-second budget plus scheduling margin')
                self.assertEqual(len(server.requests), 1)

    def test_unavailable_endpoint(self):
        # Keep the port reserved but not listening, avoiding a free-port race.
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            stdout, elapsed = run('single', 'http://127.0.0.1:%d' % reserved.getsockname()[1])
        lines = stdout.splitlines()
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], 'enabled')
        label, dropped, rejected, failed, malformed = lines[1].split()
        self.assertEqual(label, 'losses')
        dropped, rejected, failed, malformed = map(int, (dropped, rejected, failed, malformed))
        self.assertGreaterEqual(dropped, 0)
        self.assertGreaterEqual(failed, 0)
        self.assertEqual(dropped + failed, 1)
        self.assertEqual((rejected, malformed), (0, 0))
        self.assertLess(elapsed, 3.0)

    def test_custom_redaction_resource_and_attributes(self):
        with receiver() as server:
            stdout, _ = run('redaction', server.base)
            self.assertEqual(stdout, 'enabled\n')
            spans, logs = self.inspect(server, service='[CUSTOM]', suite='[CUSTOM]')
            for _, _, body, _ in server.requests:
                self.assertNotIn(b'CUSTOM_PRIVATE', body)
        self.assertEqual((len(spans), len(logs)), (2, 4))
        tool, = [s for s in spans if one(s, 5, 2) == b'tool']
        root, = [s for s in spans if one(s, 5, 2) == b'agent.run']
        context = {'session.id': 'native-session', 'run.id': one(root, 2, 2).hex()}
        self.assertEqual(attributes(tool, 9), dict(context, model='[CUSTOM]', profile='[CUSTOM]',
                                                  cancelled=False, outcome='success'))

    def test_second_state_isolation_and_retained_closure_revocation(self):
        with receiver() as server:
            stdout, _ = run('states', server.base)
            self.assertEqual(stdout, 'enabled\n')
            spans, logs = self.inspect(server)
        self.assertEqual((len(spans), len(logs)), (2, 4))
        self.assertEqual({one(s, 5, 2) for s in spans}, {b'agent.run', b'model'})
        ids = {one(s, 2, 2) for s in spans}
        self.assertEqual(len(ids), 2)
        self.assertEqual(collections.Counter((one(log, 10, 2),
                         one(wire(one(log, 5, 2)), 1, 2)) for log in logs),
                         collections.Counter({(sid, event): 1 for sid in ids
                         for event in (b'span.started', b'span.finished')}))

    def test_normal_alternation_batches_efficiently(self):
        with receiver() as server:
            stdout, elapsed = run('alternation', server.base)
            self.assertEqual(stdout, 'enabled\n')
            self.assertLess(elapsed, 3.0)
            spans, logs = self.inspect(server)
            sizes = collections.defaultdict(list)
            for path, _, body, _ in server.requests:
                records, _ = decode(body)
                self.assertLessEqual(len(body), 256 * 1024)
                sizes[path].append(len(records))
            self.assertEqual(sizes['/v1/traces'], [128, 128, 44])
            self.assertEqual(sizes['/v1/logs'], [128, 128, 128, 128, 88])
        self.assertEqual((len(spans), len(logs)), (300, 600))
        self.assertEqual([attributes(s, 9)['count'] for s in spans[1:]], list(range(2, 301)))
        ids = [one(s, 2, 2) for s in spans]
        self.assertEqual(len(set(ids)), 300)
        self.assertEqual([one(log, 10, 2) for log in logs],
                         [sid for sid in ids for _ in range(2)])
        self.assertEqual([one(wire(one(log, 5, 2)), 1, 2) for log in logs],
                         [b'span.started', b'span.finished'] * 300)

    def test_overflow_is_bounded_and_valid(self):
        with receiver() as server:
            stdout, elapsed = run('overflow', server.base)
            self.assertIn('losses 1101 0 0 0', stdout)
            self.assertLess(elapsed, 3.0)
            records = []
            for path, _, body, _ in server.requests:
                self.assertEqual(path, '/v1/traces')
                self.assertLessEqual(len(body), 256 * 1024)
                batch, _ = decode(body)
                self.assertLessEqual(len(batch), 128)
                records.extend(batch)
            self.assertEqual(len(records), 1024)
            self.assertEqual(len({one(s, 2, 2) for s in records}), 1024)


if __name__ == '__main__':
    unittest.main()
