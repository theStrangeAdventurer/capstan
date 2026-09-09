#!/usr/bin/env python3
"""Opt-in real Tempo/Loki acceptance; no server configuration changes.

make build/test_telemetry_native
python3 test/test_telemetry_grafana.py --run [--driver build/test_telemetry_native]

Focused CLI scenarios: --scenario cli-delay, cli-concurrent, or cli-retry-error.
Uses synthetic native context/roots or the real CLI modes fixture in a credential-free environment.
CLI mode uses service capstan and a unique safe session name. Delay coverage uses
two 1.25s model waits; concurrent coverage synchronizes two child HTTP requests;
retry/error coverage returns a transient 503 then success and a terminal 400.
Checks include Tempo session search, parent ownership/timelines and Loki lifecycle records.
A loopback relay captures generated IDs and forwards the original OTLP bytes to
127.0.0.1:4318. Grafana queries are restricted to those IDs. Raw backend responses
stay in memory; only validated IDs/counts are printed. No user config is read.
Context mode exercises a late child after completion/session rename, not timing.
"""
import argparse
import base64
import collections
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# Existing strict protobuf decoder; neutralize its positional-argument parsing.
_saved = sys.argv
try:
    sys.argv = [sys.argv[0]]
    from test_telemetry_otlp import decode, one, attributes
    from test_telemetry_modes import fixture, Provider, PROMPT, ANSWER
finally:
    sys.argv = _saved

ROOT = Path(__file__).resolve().parent.parent
GRAFANA = 'http://127.0.0.1:3000/api/datasources/proxy/uid/'
EXPORTER = 'http://127.0.0.1:4318'


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def safe(raw):
    require(not any(s in raw for s in ('FORBIDDEN_RAW', 'FAKE_TEST_VALUE',
                                      'mutated-session', 'completed-session')),
            'fixture redaction/context mutation leaked')


def get(path, params=None):
    url = GRAFANA + path
    if params:
        url += '?' + urllib.parse.urlencode(params)
    try:
        with HTTP.open(url, timeout=5) as response:
            raw = response.read().decode()
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            raise
        raise AssertionError('Grafana query ' + path + ': HTTP ' + str(exc.code)) from None
    safe(raw)
    return json.loads(raw)


def ident(value, size):
    if re.fullmatch('[0-9a-fA-F]{%d}' % (size * 2), value):
        return value.lower()
    raw = base64.b64decode(value, validate=True)
    require(len(raw) == size, 'backend ID size')
    return raw.hex()


def attrs(span):
    return {a['key']: next(iter(a['value'].values()))
            for a in span.get('attributes', [])}


def tempo_spans(document):
    result = []
    for batch in document.get('batches', document.get('resourceSpans', [])):
        for scope in batch.get('scopeSpans', batch.get('instrumentationLibrarySpans', [])):
            result.extend(scope.get('spans', []))
    return result


class Relay(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            require(self.path in ('/v1/traces', '/v1/logs'), 'unexpected export route')
            body = self.rfile.read(int(self.headers['Content-Length']))
            safe(body.decode('utf-8', errors='ignore'))
            for private in (PROMPT, ANSWER, 'FIXTURE_TOOL_PRIVATE', 'fixture-not-a-secret'):
                require(private.encode() not in body, 'private fixture content exported')
            records, resources = decode(body)
            if getattr(self.server, 'service', None):
                require(all(r.get('service.name') == self.server.service for r in resources),
                        'CLI service resource')
            if getattr(self.server, 'outage', False):
                attempts = self.server.attempts[self.path]
                attempts.append(body)
                if sum(map(len, self.server.attempts.values())) == 1:
                    self.server.rejected_path = self.path
                    # Fail before forwarding: no ambiguous collector acknowledgement.
                    self.send_response(503)
                    self.send_header('Retry-After', '0')
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                require(body == attempts[0], 'recovery retry changed protobuf payload')
            with HTTP.open(urllib.request.Request(EXPORTER + self.path, data=body,
                           headers={'Content-Type': 'application/x-protobuf'}), timeout=5) as r:
                reply = r.read()
                status = r.status
            self.server.records[self.path].extend(records)
            self.send_response(status)
            self.send_header('Content-Length', str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)
        except Exception:
            self.server.failed = True
            self.send_error(502, 'fixture export failed')


def poll(callback, description):
    deadline = time.monotonic() + 60
    while True:
        try:
            result = callback()
            if result:
                return result
        except urllib.error.HTTPError as exc:
            if exc.code != 404:
                raise
        require(time.monotonic() < deadline, description + ' timed out')
        time.sleep(1)


def validate_trace(trace, expected, start, service='native-test', delay=0,
                   expected_resources=None):
    document = poll(lambda: (d if len(tempo_spans(d)) >= len(expected) else None)
                    if (d := get('tempo/api/traces/' + trace)) else None, 'Tempo ingestion')
    spans = tempo_spans(document)
    if expected_resources:
        batches = document.get('batches', document.get('resourceSpans', []))
        require(bool(batches), 'Tempo resource batches')
        for batch in batches:
            resource = attrs(batch.get('resource', {}))
            require({k: v for k, v in resource.items() if k.startswith('benchmark.')} ==
                    expected_resources, 'Tempo benchmark resource identity')
    require(len(spans) == len(expected), 'Tempo span count')
    by_id = {ident(s['spanId'], 8): s for s in spans}
    require(set(by_id) == set(expected), 'Tempo span IDs')
    for sid, wire_span in expected.items():
        span = by_id[sid]
        require(ident(span['traceId'], 16) == trace, 'Tempo trace ID')
        require(span['name'] == one(wire_span, 5, 2).decode(), 'Tempo name')
        require(attrs(span) == attributes(wire_span, 9), 'Tempo snapshot attributes')
        parent = one(wire_span, 4, 2).hex() if 4 in wire_span else ''
        require((ident(span['parentSpanId'], 8) if span.get('parentSpanId') else '') == parent,
                'Tempo parent')
        require(0 < int(span['startTimeUnixNano']) <= int(span['endTimeUnixNano']),
                'Tempo timestamps')

    def logs():
        doc = get('loki/loki/api/v1/query_range', {
            'query': '{service_name="' + service + '"} | trace_id="' + trace + '"',
            'start': str(start), 'end': str(time.time_ns()), 'limit': '1000'})
        require(doc['status'] == 'success', 'Loki query status')
        rows = [(stream['stream'], value) for stream in doc['data']['result']
                for value in stream['values']]
        return rows if len(rows) >= 2 * len(expected) else None

    rows = poll(logs, 'Loki ingestion')
    require(len(rows) == 2 * len(expected), 'Loki log count')
    events = collections.Counter()
    timestamps = {}
    for labels, value in rows:
        metadata = dict(labels)
        if len(value) > 2:
            metadata.update(value[2])
        if expected_resources:
            require({k: v for k, v in metadata.items() if k.startswith('benchmark_')} ==
                    {k.replace('.', '_'): v for k, v in expected_resources.items()},
                    'Loki benchmark resource identity')
        require(metadata.get('trace_id') == trace, 'Loki trace correlation')
        sid = metadata.get('span_id')
        require(sid in by_id, 'Loki span correlation')
        event = value[1]
        require(event in ('span.started', 'span.finished'), 'Loki lifecycle body')
        events[sid, event] += 1
        timestamps[sid, event] = int(value[0])
        snapshot = attrs(by_id[sid])
        for key in ('session.id', 'session.name', 'mode', 'run.id'):
            normalized = key.replace('.', '_')
            require(metadata.get(normalized) == snapshot.get(key), 'Loki snapshot ' + key)
        require(metadata.get('span_name') == by_id[sid]['name'], 'Loki span name')
        if event == 'span.finished':
            require(metadata.get('outcome') == snapshot.get('outcome'), 'Loki outcome')
    require(events == collections.Counter({(sid, event): 1 for sid in by_id
            for event in ('span.started', 'span.finished')}), 'Loki event multiplicity')
    if delay:
        root, = [s for s in spans if s['name'] == 'agent.run']
        models = [s for s in spans if s['name'] == 'agent.model']
        require(len(models) == 2, 'CLI model count')
        for model in models:
            sid = ident(model['spanId'], 8)
            begin, end = int(model['startTimeUnixNano']), int(model['endTimeUnixNano'])
            require(end - begin >= delay * 1e9, 'Tempo controlled model delay')
            require(int(root['startTimeUnixNano']) <= begin <= end <=
                    int(root['endTimeUnixNano']), 'Tempo root contains model timeline')
            require(timestamps[sid, 'span.finished'] - timestamps[sid, 'span.started'] >=
                    delay * 1e9, 'Loki controlled model delay')
    return {'trace_id': trace, 'span_ids': sorted(by_id), 'spans': len(spans),
            'logs': len(rows), 'snapshots': [
                {k: v for k, v in attrs(by_id[sid]).items()
                 if k in ('session.id', 'session.name', 'mode', 'run.id')}
                for sid in sorted(by_id)]}


class DelayedProvider(Provider):
    def do_POST(self):
        time.sleep(1.25)
        super().do_POST()


class SubagentProvider(Provider):
    """Real tool dispatch, with two independently served child HTTP requests."""

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        messages = request['messages']
        child = next((name for name in ('fixture-child-a', 'fixture-child-b')
                      if any(m.get('role') == 'user' and name in str(m.get('content'))
                             for m in messages)), None)
        with self.server.fixture_lock:
            self.server.requests.append(request)
            self.server.calls[child] += 1
            attempt = self.server.calls[child]
        if child:
            if self.server.scenario == 'cli-concurrent':
                # A sequential implementation cannot pass this rendezvous.
                try:
                    self.server.children_ready.wait(timeout=5)
                except threading.BrokenBarrierError:
                    self.server.fixture_failed = True
                    self.send_error(500, 'fixture rendezvous timeout')
                    return
                time.sleep(1.25)
            elif child == 'fixture-child-b' or attempt == 1:
                body = (b'{"error":{"message":"fixture failure"},'
                        b'"private_debug":"FORBIDDEN_RAW"}')
                self.send_response(400 if child == 'fixture-child-b' else 503)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            delta = {'content': ANSWER}
        elif any(m.get('role') == 'tool' for m in messages):
            delta = {'content': ANSWER}
        else:
            delta = {'tool_calls': [{'index': 0, 'id': 'fixture_subagents',
                     'type': 'function', 'function': {'name': 'subagents',
                     'arguments': json.dumps({'max_concurrent': 2, 'tasks': [
                         {'id': name, 'task': name, 'tools': [], 'max_turns': 1}
                         for name in ('fixture-child-a', 'fixture-child-b')]})}}]}
        chunks = [{'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]},
                  {'choices': [{'index': 0, 'delta': {}, 'finish_reason':
                    'tool_calls' if 'tool_calls' in delta else 'stop'}]}]
        body = (''.join('data: ' + json.dumps(c) + '\n\n' for c in chunks) +
                'data: [DONE]\n\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def validate_subagents(spans, scenario):
    def named(name):
        return [s for s in spans if one(s, 5, 2) == name.encode()]

    def timestamp(s, field):
        return int.from_bytes(one(s, field, 1), 'little')

    roots = [s for s in named('agent.run') if 4 not in s]
    require(len(roots) == 1, 'CLI orchestrator root count')
    root = roots[0]
    tool, = named('agent.tool')
    require(attributes(tool, 9).get('tool') == 'subagents', 'CLI subagents dispatch')
    require(one(tool, 4, 2) == one(root, 2, 2), 'CLI subagents tool owner')
    children = named('subagent')
    require(len(children) == 2, 'CLI child run count')
    for child in children:
        require(one(child, 4, 2) == one(tool, 2, 2), 'CLI child owner')
        require(timestamp(tool, 7) <= timestamp(child, 7) <= timestamp(child, 8) <=
                timestamp(tool, 8), 'CLI tool contains child')
        models = [s for s in named('agent.model') if one(s, 4, 2) == one(child, 2, 2)]
        require(bool(models), 'CLI child model ownership')
        for model in models:
            require(timestamp(child, 7) <= timestamp(model, 7) <= timestamp(model, 8) <=
                    timestamp(child, 8), 'CLI child contains model')
    if scenario == 'cli-concurrent':
        require(len(named('agent.model')) == 4, 'CLI concurrent model count')
        require(all(attributes(s, 9)['outcome'] == 'success' for s in spans),
                'CLI concurrent outcomes')
        require(min(timestamp(c, 8) for c in children) -
                max(timestamp(c, 7) for c in children) >= 1.25e9,
                'CLI child runs overlap during controlled wait')
    else:
        require(len(named('agent.model')) == 5, 'CLI retry model count')
        require(collections.Counter(attributes(c, 9)['outcome'] for c in children) ==
                {'success': 1, 'error': 1}, 'CLI child success/error outcomes')
        retries = [s for s in named('operation')
                   if attributes(s, 9).get('operation') == 'retry']
        require(len(retries) == 1 and attributes(retries[0], 9).get('purpose') ==
                'stream_transient_error', 'CLI transient stream retry')
        recovered = next(c for c in children if attributes(c, 9)['outcome'] == 'success')
        require(one(retries[0], 4, 2) == one(recovered, 2, 2), 'CLI retry owner')
        models = sorted([s for s in named('agent.model')
                         if one(s, 4, 2) == one(recovered, 2, 2)],
                        key=lambda s: timestamp(s, 7))
        require([attributes(s, 9)['outcome'] for s in models] == ['error', 'success'],
                'CLI failed attempt then recovery')
        require(timestamp(models[0], 8) <= timestamp(retries[0], 7) <=
                timestamp(retries[0], 8) <= timestamp(models[1], 7),
                'CLI retry timeline order')
    return root


def cli_acceptance(binary, benchmark=False, scenario='cli-delay'):
    server = http.server.HTTPServer(('127.0.0.1', 0), Relay)
    server.service = 'capstan'
    server.records = collections.defaultdict(list)
    server.failed = False
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    start = time.time_ns() - 1_000_000_000
    subagents = scenario in ('cli-concurrent', 'cli-retry-error')
    session = 'grafana-' + scenario + '-' + str(time.time_ns())
    expected_resources = None
    if benchmark:
        # Use the canonical harness adapter, never run evaluations or inherit config.
        spec = importlib.util.spec_from_file_location(
            'grafana_benchmark_adapter', ROOT / 'benchmarks/polyglot/scripts/run_eval.py')
        consumer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(consumer)
        identity = {'attempt_id': session, 'config_id': 'grafana-fixture',
                    'harness_sha256': 'a' * 64, 'task': 'synthetic/resource-check',
                    'comparison_id': 'grafana-acceptance', 'replicate_id': 'r1'}
        expected_resources = {'benchmark.' + k: v for k, v in identity.items()}
    try:
        with fixture(include_session_name=True) as (root, work, env, args, provider, _):
            # Only the disposable fixture configuration beneath build is touched.
            config = root / 'home/.config/capstan/config.lua'
            text = config.read_text()
            text = text.replace('service_name = "modes-test"', 'service_name = "capstan"')
            text = re.sub(r'(observability = \{enabled = true, endpoint = ")[^"]+',
                          r'\g<1>http://127.0.0.1:%d' % server.server_port, text)
            config.write_text(text)
            if subagents:
                provider.scenario = scenario
                provider.fixture_lock = threading.Lock()
                provider.calls = collections.Counter()
                provider.children_ready = threading.Barrier(2)
                provider.fixture_failed = False
                provider.RequestHandlerClass = SubagentProvider
            else:
                provider.RequestHandlerClass = DelayedProvider
            args[args.index('--session-id') + 1] = session
            if benchmark:
                env.update(consumer.telemetry_environment(
                    identity, 'benchmark.attempt_id=stale,benchmark.untrusted=drop'))
            proc = subprocess.run([str(binary), 'run', *args, '--json', '--prompt', PROMPT],
                                  cwd=work, env=env, capture_output=True, text=True, timeout=25)
            require(proc.returncode == 0, 'CLI fixture failed')
            require(json.loads(proc.stdout) == {'ok': True, 'text': ANSWER, 'error': ''},
                    'CLI fixture answer')
            if subagents:
                require(not provider.fixture_failed, 'CLI concurrent rendezvous')
                require(provider.calls == {None: 2, 'fixture-child-a':
                        2 if scenario == 'cli-retry-error' else 1, 'fixture-child-b': 1},
                        'CLI child request/retry counts')
                tool_messages = [m for m in provider.requests[-1]['messages']
                                 if m.get('role') == 'tool']
                require(len(tool_messages) == 1, 'CLI parent receives batch result')
                result = json.loads(tool_messages[0]['content'])
                require([r['id'] for r in result['results']] ==
                        ['fixture-child-a', 'fixture-child-b'], 'CLI ordered child results')
                require([r['ok'] for r in result['results']] ==
                        [True, scenario == 'cli-concurrent'], 'CLI child result outcomes')
                safe(json.dumps(provider.requests[-1]))
            else:
                require(len(provider.requests) == 2, 'CLI provider requests')
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    require(not server.failed, 'CLI OTLP relay failure')
    spans = server.records['/v1/traces']
    if subagents:
        root = validate_subagents(spans, scenario)
    else:
        require(collections.Counter(one(s, 5, 2) for s in spans) ==
                {b'agent.run': 1, b'agent.model': 2, b'agent.tool': 1, b'operation': 3},
                'CLI exported span structure')
        root, = [s for s in spans if one(s, 5, 2) == b'agent.run']
    require(4 not in root, 'CLI run must be a root')
    root_measurements = attributes(root, 9)
    require(root_measurements['request_count'] == 2 and root_measurements['tool_count'] == 1,
            'root counts exclude child attempts')
    direct_models = [s for s in spans if one(s, 5, 2) == b'agent.model'
                     and one(s, 4, 2) == one(root, 2, 2)]
    require(root_measurements['model_ms'] == sum(attributes(s, 9)['duration_ms']
                                                for s in direct_models),
            'root model total uses common measurements')
    require(abs(root_measurements['duration_ms'] - sum(root_measurements[k] for k in (
        'model_ms', 'tool_ms', 'permission_wait_ms', 'subagent_wait_ms', 'unattributed_ms'))
        + root_measurements['overlap_ms']) < 0.001, 'root duration accounting')
    trace = one(root, 1, 2).hex()
    for span in spans:
        snapshot = attributes(span, 9)
        require(one(span, 1, 2).hex() == trace and snapshot['mode'] == 'cli' and
                snapshot['session.id'] == session and snapshot['session.name'] == session,
                'CLI session context')
    evidence = validate_trace(trace, {one(s, 2, 2).hex(): s for s in spans}, start,
                              service='capstan', delay=0 if subagents else 1.25,
                              expected_resources=expected_resources)
    for key in ('session.id', 'session.name'):
        query = '{ resource.service.name = "capstan" && span.' + key + ' = "' + session + '" }'
        def search():
            # Unique session scopes the ingester search; explicit time bounds
            # switch Tempo to block search, which may reject very recent ranges.
            document = get('tempo/api/search', {'q': query})
            return any(ident(t['traceID'], 16) == trace for t in document.get('traces', []))
        poll(search, 'Tempo session search ' + key)
    if benchmark:
        evidence['benchmark_resources'] = expected_resources
    return dict(mode='benchmark-resources' if benchmark else scenario,
                session_id=session, session_name=session,
                model_delay_ms=0 if scenario == 'cli-retry-error' else 1250, **evidence)


def dashboard_acceptance(live=False):
    """Check the shipped selector contract; optionally query the installed plugin/backends.

    Read-only: no dashboard import, fixture export, credentials or raw values printed.
    The formatter mirrors Grafana's single-value :regex and :doublequote formats.
    """
    dashboard = json.loads((ROOT / 'examples/observability/capstan-dashboard.json').read_text())
    variables = {v['name']: v for v in dashboard['templating']['list']}
    panels = {p['id']: p for p in dashboard['panels']}
    require(len(panels) == len(dashboard['panels']), 'unique dashboard panel IDs')
    for key in ('session_id', 'session_name'):
        v = variables[key]
        require(v['type'] == 'query' and v['datasource'] ==
                {'type': 'tempo', 'uid': '${tempo}'}, 'Tempo query selector')
        require(v['query']['type'] == 1 and v['query']['label'] ==
                'span.' + key.replace('_', '.'), 'Tempo label-values schema')
        require(v['refresh'] == 2 and not v['multi'] and v['includeAll'] and
                v['allValue'] == '.*' and v['current']['value'] == '$__all',
                'single literal selection with All default')
        require(v['regex'] == '/^[^`]*$/', 'raw-string delimiter excluded from options')
        for query in (panels[3]['targets'][0]['query'], panels[4]['targets'][0]['expr']):
            require('^${' + key + ':regex}$' in query, 'shared anchored session predicate')
        for pid in (2, 5):
            require('${' + key not in panels[pid]['targets'][0]['query'],
                    'unfiltered legacy/benchmark fallback')
    require(panels[4]['targets'][0]['expr'].startswith('{service_name="capstan"} | '),
            'session fields remain metadata, not indexed labels')

    def regex(value):
        # Grafana escapeStringForRegex (not Python re.escape's additional escapes).
        return re.sub(r'([|\\{}()\[\]^$+*?.])', r'\\\1', value)

    def expand(query, selected=None):
        selected = selected or {}
        def substitute(match):
            key, fmt = match.groups()
            value = selected.get(key, variables[key].get('allValue', variables[key]['query']))
            if fmt == 'regex':
                return regex(value) if key in selected else value
            require(fmt == 'doublequote', 'known dashboard formatter')
            return '"' + value.replace('"', '\\"') + '"'
        return re.sub(r'\$\{(\w+):(\w+)\}', substitute, query)

    for value in ('release.1', 'same [name]', 'a"b\\c', '.*', 'имя (новое)'):
        pattern = '^' + regex(value) + '$'
        require(re.fullmatch(pattern, value) is not None and
                re.fullmatch(pattern, 'prefix' + value) is None,
                'literal punctuation/Unicode and no substring false positive')
    # Stable identity has no dependency on either historical or current name.
    stable = expand(panels[3]['targets'][0]['query'], {'session_id': 'stable-id'})
    require('`^stable-id$`' in stable and '`.*` = `.*`' in stable,
            'ID plus name All includes renamed/unnamed history')
    if not live:
        return {'result': 'PASS', 'dashboard': 'static'}

    with HTTP.open('http://127.0.0.1:3000/public/plugins/tempo/module.js', timeout=10) as response:
        plugin = response.read().decode()
    require('LabelValues=1' in plugin and 'switch(e.type)' in plugin and
            'this.labelValuesQuery(e.label,t)' in plugin,
            'installed Tempo plugin variable schema changed; inspect before updating')
    counts = {}
    for key in ('session_id', 'session_name'):
        tag = variables[key]['query']['label']
        document = get('tempo/api/v2/search/tag/' + tag + '/values',
                       {'q': '{ resource.service.name = "capstan" }'})
        values = [v['value'] for v in document.get('tagValues', [])
                  if v.get('type') == 'string' and '`' not in v.get('value', '')]
        counts[key] = len(values)
    cases = ({}, {'session_id': 'capstan-no-such-session-9e163451'},
             {'session_name': 'capstan-no-such-session-9e163451'},
             {'session_name': 'release.1 [test] "quoted" \\ path'})
    for selected in cases:
        query = expand(panels[3]['targets'][0]['query'], selected)
        traces = get('tempo/api/search', {'q': query, 'limit': 100})
        logs = get('loki/loki/api/v1/query_range', {
            'query': expand(panels[4]['targets'][0]['expr'], selected),
            'since': '24h', 'limit': 1000})
        require(logs['status'] == 'success', 'dashboard Loki query')
        if selected:
            require(not traces.get('traces') and not logs['data']['result'],
                    'nonexistent literal selection must not broaden search')
    return {'result': 'PASS', 'dashboard': 'installed Tempo schema + read-only queries',
            'option_counts': counts, 'browser_import': 'not tested',
            'rename_fixture': 'not exported; ID predicate checked statically'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true', help='explicitly export safe fixtures')
    parser.add_argument('--driver', default='build/test_telemetry_native')
    cli_scenarios = ('cli-delay', 'benchmark-resources', 'cli-concurrent', 'cli-retry-error')
    parser.add_argument('--scenario', choices=('native', 'failure', 'backend-recovery', 'all', *cli_scenarios),
                        default='native')
    parser.add_argument('--binary', default='build/capstan')
    parser.add_argument('--dashboard-check', action='store_true',
                        help='check dashboard; --run adds read-only installed plugin/backend checks')
    args = parser.parse_args()
    if args.dashboard_check:
        print(json.dumps(dashboard_acceptance(live=args.run), indent=2))
        return
    if not args.run:
        print('SKIP: real backend acceptance requires --run')
        return
    driver = Path(args.driver).resolve()
    if args.scenario not in cli_scenarios:
        require(driver.is_relative_to(ROOT) and driver.is_file(), 'driver must exist in workspace')
    env = {'PATH': os.defpath, 'LC_ALL': 'C', 'NO_PROXY': '*', 'no_proxy': '*'}
    evidence = []
    if args.scenario in (*cli_scenarios, 'all'):
        binary = Path(args.binary).resolve()
        require(binary.is_relative_to(ROOT) and binary.is_file(), 'binary must exist in workspace')
        if args.scenario in ('cli-delay', 'all'):
            evidence.append(cli_acceptance(binary))
        if args.scenario in ('benchmark-resources', 'all'):
            evidence.append(cli_acceptance(binary, benchmark=True))
        for scenario in ('cli-concurrent', 'cli-retry-error'):
            if args.scenario in (scenario, 'all'):
                evidence.append(cli_acceptance(binary, scenario=scenario))
    modes = {**dict.fromkeys(cli_scenarios, ()), 'native': ('context', 'roots'),
             'failure': ('failure',), 'backend-recovery': ('backend-recovery',),
             'all': ('context', 'roots', 'failure', 'backend-recovery')}
    for mode in modes[args.scenario]:
        server = http.server.HTTPServer(('127.0.0.1', 0), Relay)
        server.records = collections.defaultdict(list)
        server.outage = mode == 'backend-recovery'
        server.attempts = collections.defaultdict(list)
        server.failed = False
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        start = time.time_ns() - 1_000_000_000
        try:
            result = subprocess.run([str(driver), 'failure' if server.outage else mode,
                                     'http://127.0.0.1:%d' % server.server_port],
                                    cwd=ROOT, env=env, capture_output=True, timeout=15)
            require(result.returncode == 0 and result.stdout == b'enabled\n' and not result.stderr,
                    'native fixture failed or reported exporter losses')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        require(not server.failed, 'OTLP relay failure')
        spans = server.records['/v1/traces']
        require(len(spans) == (4 if mode == 'roots' else 3), 'fixture span count')
        if server.outage:
            require({path: len(bodies) for path, bodies in server.attempts.items()} ==
                    {path: 2 if path == server.rejected_path else 1
                     for path in ('/v1/traces', '/v1/logs')}, 'recovery export attempt counts')
            require(len({(one(s, 1, 2), one(s, 2, 2)) for s in spans}) == len(spans),
                    'duplicate successfully forwarded spans')
            logs = server.records['/v1/logs']
            require(len({(one(log, 9, 2), one(log, 10, 2), one(log, 5, 2))
                         for log in logs}) == len(logs),
                    'duplicate successfully forwarded lifecycle records')
        if mode in ('failure', 'backend-recovery'):
            require({one(s, 5, 2).decode(): attributes(s, 9)['outcome'] for s in spans} ==
                    {'agent.run': 'success', 'agent.model': 'error', 'agent.tool': 'cancelled'},
                    'native synthetic failure/cancellation outcomes')
        require(len(server.records['/v1/logs']) == len(spans) * 2, 'fixture log count')
        traces = collections.defaultdict(dict)
        for span in spans:
            snapshot = attributes(span, 9)
            if mode == 'context':
                require(snapshot['session.id'] == 'native-session' and
                        snapshot['session.name'] == 'api_key=[REDACTED]' and
                        snapshot['mode'] == 'tui', 'immutable late-child context')
            traces[one(span, 1, 2).hex()][one(span, 2, 2).hex()] = span
        if mode == 'roots':
            require(len(traces) == 3, 'new roots need independent traces')
            require({attributes(s, 9).get('session.id') for s in spans} ==
                    {'native-session', 'second-session', None}, 'new root session snapshots')
            require(all('session.name' not in attributes(s, 9) for s in spans),
                    'session names must be opt-in')
        for trace, expected in traces.items():
            entry = dict(mode=mode, **validate_trace(trace, expected, start))
            if server.outage:
                entry['exports'] = {path: {'attempts': len(bodies),
                                          'rejected_503': int(path == server.rejected_path),
                                          'forwarded_successfully': 1}
                                    for path, bodies in server.attempts.items()}
            evidence.append(entry)
    print(json.dumps({'result': 'PASS', 'traces': len(evidence),
                      'spans': sum(e['spans'] for e in evidence),
                      'logs': sum(e['logs'] for e in evidence), 'evidence': evidence}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        # Never print raw HTTP responses, arbitrary subprocess output or credentials.
        print('FAIL Grafana acceptance: ' + type(exc).__name__, file=sys.stderr)
        if isinstance(exc, AssertionError):
            print(str(exc), file=sys.stderr)
        sys.exit(1)
