#!/usr/bin/env python3
"""Real Collector acceptance test (Python standard library only).

Run: python3 test/test_telemetry_collector.py COLLECTOR_PATH DRIVER_PATH
Uses the native driver's sample contract and Collector 0.160.0. All generated
configuration, output and diagnostics remain in a unique workspace build folder.
No inherited configuration, credentials, proxies or Lua startup environment.
"""
import collections
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def records(path, resource_key, scope_key, record_key):
    result = []
    for line in path.read_text(encoding='utf-8').splitlines():
        document = json.loads(line)
        for resource in document[resource_key]:
            attrs = resource['resource']['attributes']
            require(len(attrs) == 3, 'resource attribute cardinality')
            require({a['key']: a['value'] for a in attrs} == {
                'service.name': {'stringValue': 'native-test'},
                'service.version': {'stringValue': 'test-1'},
                'suite': {'stringValue': 'native'},
            }, 'resource service/version/suite mismatch')
            for scope in resource[scope_key]:
                require(scope['scope']['name'] == 'capstan.native'
                        and scope['scope']['version'] == '1', 'scope mismatch')
                result.extend(scope[record_key])
    return result


def validate(traces_path, logs_path):
    spans = records(traces_path, 'resourceSpans', 'scopeSpans', 'spans')
    logs = records(logs_path, 'resourceLogs', 'scopeLogs', 'logRecords')
    require(len(spans) == 3 and len(logs) == 6, 'expected exactly 3 spans and 6 logs')
    by_name = {span['name']: span for span in spans}
    require(set(by_name) == {'agent.run', 'agent.model', 'agent.tool'}, 'span names')
    root, model, tool = (by_name[n] for n in ('agent.run', 'agent.model', 'agent.tool'))
    trace_id = root['traceId']
    require(re.fullmatch(r'[0-9a-f]{32}', trace_id) and int(trace_id, 16), 'trace ID')
    require(root.get('parentSpanId', '') in ('', '0' * 16), 'root has parent')
    require(model['parentSpanId'] == root['spanId'], 'model parent ID')
    require(tool['parentSpanId'] == model['spanId'], 'tool parent ID')
    ids = {s['spanId'] for s in spans}
    require(len(ids) == 3, 'span IDs must be unique')
    for span in spans:
        require(re.fullmatch(r'[0-9a-f]{16}', span['spanId'])
                and int(span['spanId'], 16), 'span ID')
        require(span['traceId'] == trace_id, 'span trace correlation')
        require(0 < int(span['startTimeUnixNano']) <= int(span['endTimeUnixNano']),
                'span timestamps')
    events = collections.Counter()
    for log in logs:
        require(log['traceId'] == trace_id and log['spanId'] in ids, 'log correlation')
        event = log['body']['stringValue']
        events[log['spanId'], event] += 1
        require(int(log['timeUnixNano']) > 0, 'log timestamp')
        error = log['spanId'] == model['spanId'] and event == 'span.finished'
        require(log['severityNumber'] == (17 if error else 9)
                and log['severityText'] == ('ERROR' if error else 'INFO'), 'log severity')
    require(events == collections.Counter({(sid, event): 1 for sid in ids
            for event in ('span.started', 'span.finished')}), 'lifecycle log multiplicity')
    for path in (traces_path, logs_path):
        raw = path.read_text(encoding='utf-8')
        require('FORBIDDEN_RAW' not in raw and 'FAKE_TEST_VALUE' not in raw,
                'unredacted fixture data')


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError('Collector shutdown timed out')


def main():
    require(len(sys.argv) == 3, 'usage: test_telemetry_collector.py COLLECTOR_PATH DRIVER_PATH')
    collector, driver = (Path(arg).resolve() for arg in sys.argv[1:])
    for binary in (collector, driver):
        require(binary.is_relative_to(ROOT) and binary.is_file(),
                'binaries must exist inside the workspace')
    build = ROOT / 'build'
    require(build.resolve().is_relative_to(ROOT), 'build must stay inside workspace')
    build.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='telemetry-collector-', dir=build))
    env = {'PATH': os.defpath, 'LC_ALL': 'C', 'NO_PROXY': '*', 'no_proxy': '*',
           'HOME': str(work), 'TMPDIR': str(work), 'XDG_CONFIG_HOME': str(work)}
    version = subprocess.run([str(collector), '--version'], cwd=work, env=env,
                             capture_output=True, text=True, timeout=10, check=True)
    require(re.search(r'\b0\.160\.0\b', version.stdout), 'expected Collector 0.160.0')
    # Reserve an ephemeral loopback port until immediately before Collector starts.
    # Collector startup failure (including a bind race) is surfaced, never ignored.
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    traces, logs = work / 'traces.json', work / 'logs.json'
    config = work / 'collector.yaml'
    config.write_text(f'''receivers:
  otlp:
    protocols:
      http:
        endpoint: "127.0.0.1:{port}"
exporters:
  file/traces:
    path: {json.dumps(str(traces))}
    format: json
  file/logs:
    path: {json.dumps(str(logs))}
    format: json
service:
  telemetry:
    metrics:
      level: none
  pipelines:
    traces:
      receivers: [otlp]
      exporters: [file/traces]
    logs:
      receivers: [otlp]
      exporters: [file/logs]
''', encoding='utf-8')
    with (work / 'collector.log').open('wb') as diagnostic:
        process = subprocess.Popen([str(collector), '--config', str(config)],
                                   cwd=work, env=env, stdin=subprocess.DEVNULL,
                                   stdout=diagnostic, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 15
            while True:
                require(process.poll() is None, 'Collector exited during startup; see collector.log')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.2):
                        break
                except OSError:
                    require(time.monotonic() < deadline, 'Collector readiness timed out')
                    time.sleep(0.05)
            sample = subprocess.run([str(driver), 'sample', f'http://127.0.0.1:{port}'],
                                    cwd=work, env=env, capture_output=True,
                                    text=True, timeout=12)
            require(sample.returncode == 0 and sample.stdout == 'enabled\n'
                    and not sample.stderr, 'native sample failed or reported losses')
            require(process.poll() is None, 'Collector exited during sample')
        finally:
            stop(process)
    require(process.returncode == 0, 'Collector shutdown failed; see collector.log')
    validate(traces, logs)
    print(f'PASS Collector 0.160.0: 3 spans, 6 logs; parents, correlation, resource OK ({work.relative_to(ROOT)})')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        # Do not echo arbitrary subprocess output or serialized telemetry on failure.
        print(f'FAIL Collector acceptance: {type(exc).__name__}', file=sys.stderr)
        sys.exit(1)
