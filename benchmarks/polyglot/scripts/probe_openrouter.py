#!/usr/bin/env python3
"""Controlled OpenRouter route/attribution probe; not a coding benchmark.

Same model, prompt, tools, reasoning and output limit in every case. Vary only
routing preference and app attribution. No retries and no credential logging.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time
import urllib.request

URL = 'https://openrouter.ai/api/v1/chat/completions'
PROMPT = ('Implement a Python LRU cache class with get and put, O(1) operations, '
          'using a dictionary and a doubly linked list. Include three small usage '
          'examples. Return the complete code, without running tools.')
CASES = [
    ('unattributed-auto', None, False),
    ('opencode-auto', None, True),
    ('unattributed-throughput', {'sort': 'throughput'}, False),
    ('opencode-throughput', {'sort': 'throughput'}, True),
]


def request_body(model: str, routing: dict | None) -> dict:
    body = {
        'model': model,
        'messages': [{'role': 'user', 'content': PROMPT}],
        'reasoning': {'effort': 'medium'}, 'max_tokens': 32000,
        'stream': True, 'stream_options': {'include_usage': True},
        'tools': [{'type': 'function', 'function': {'name': 'read_file',
            'description': 'Read a file.', 'parameters': {'type': 'object',
                'properties': {'path': {'type': 'string'}}, 'required': ['path']}}}],
        'tool_choice': 'auto',
    }
    if routing is not None:
        body['provider'] = routing
    return body


def read_stream(response, started: float, clock=time.monotonic) -> dict:
    """read1 avoids buffering until a large read is full (which distorts TTFT)."""
    result = {'response_id': None, 'upstream_provider': None, 'response_model': None,
              'first_output_seconds': None, 'usage': None, 'finished': False}
    buf = b''

    def event(raw):
        data = b'\n'.join(line[5:].lstrip() for line in raw.split(b'\n') if line.startswith(b'data:'))
        if not data:
            return
        if data == b'[DONE]':
            result['finished'] = True
            return
        value = json.loads(data)
        if value.get('error'):
            # Preserve the record, but don't print arbitrary upstream error bodies.
            result['error'] = 'provider_error'
        for source, target in [('id', 'response_id'), ('provider', 'upstream_provider'), ('model', 'response_model')]:
            if isinstance(value.get(source), str):
                result[target] = value[source]
        if isinstance(value.get('usage'), dict):
            result['usage'] = value['usage']
        choices = value.get('choices') or []
        if choices:
            delta = choices[0].get('delta') or {}
            if any(delta.get(k) for k in ('content', 'reasoning', 'reasoning_content', 'reasoning_details', 'tool_calls')):
                if result['first_output_seconds'] is None:
                    result['first_output_seconds'] = clock() - started
            if choices[0].get('finish_reason'):
                result['finish_reason'] = choices[0]['finish_reason']

    while True:
        chunk = response.read1(65536)
        if not chunk:
            break
        buf += chunk
        # Leave a split CRLF intact until the next read.
        trailing = buf.endswith(b'\r')
        buf = (buf[:-1] if trailing else buf).replace(b'\r\n', b'\n').replace(b'\r', b'\n') + (b'\r' if trailing else b'')
        while b'\n\n' in buf:
            raw, buf = buf.split(b'\n\n', 1)
            event(raw)
    if buf.strip():
        event(buf.replace(b'\r\n', b'\n'))
    result['seconds'] = clock() - started
    usage = result['usage'] or {}
    tokens = usage.get('completion_tokens')
    first = result['first_output_seconds']
    if isinstance(tokens, (int, float)) and first is not None and result['seconds'] > first:
        result['completion_tokens_per_second_after_first_output'] = tokens / (result['seconds'] - first)
    if not result['finished']:
        result['error'] = result.get('error', 'incomplete_stream')
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New JSONL output file')
    parser.add_argument('--model', default='deepseek/deepseek-v4-pro')
    parser.add_argument('--repeats', type=int, default=1, help='4 paid calls per repetition')
    parser.add_argument('--timeout', type=float, default=240)
    parser.add_argument('--only', help='Pin the same exact upstream slug in all cases; disables fallback')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if args.repeats < 1 or args.timeout <= 0:
        parser.error('repeats and timeout must be positive')
    key = os.environ.get('OPENROUTER_API_KEY')
    if not key and not args.dry_run:
        parser.error('OPENROUTER_API_KEY is required')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x', encoding='utf-8') as output:
        for repeat in range(args.repeats):
            # Reverse on alternate repetitions to reduce monotonic time effects.
            cases = CASES if repeat % 2 == 0 else list(reversed(CASES))
            for name, preference, attributed in cases:
                routing = dict(preference or {})
                if args.only:
                    routing.update(only=[args.only], allow_fallbacks=False)
                body = request_body(args.model, routing or None)
                record = {'case': name, 'repeat': repeat + 1, 'request': body,
                          'opencode_attribution': attributed, 'dry_run': args.dry_run}
                if not args.dry_run:
                    headers = {'Authorization': 'Bearer ' + key, 'Content-Type': 'application/json'}
                    if attributed:
                        headers.update({'HTTP-Referer': 'https://opencode.ai/', 'X-Title': 'opencode'})
                    request = urllib.request.Request(URL, data=json.dumps(body).encode(), headers=headers)
                    started = time.monotonic()
                    try:
                        with urllib.request.urlopen(request, timeout=args.timeout) as response:
                            record.update(read_stream(response, started))
                    except Exception as error:
                        record.update(error=type(error).__name__, http_status=getattr(error, 'code', None))
                output.write(json.dumps(record, ensure_ascii=False) + '\n')
                output.flush()
                print(json.dumps({k: record[k] for k in ('case', 'repeat', 'upstream_provider', 'response_id',
                    'seconds', 'completion_tokens_per_second_after_first_output', 'error') if k in record}), flush=True)
                if record.get('error'):
                    return 1  # Never retry auth, billing or transport failures blindly.
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
