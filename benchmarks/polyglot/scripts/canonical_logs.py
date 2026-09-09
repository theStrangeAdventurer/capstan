"""Strict optional capstan.log.v1 adapter. No instrumentation or clock inference."""
import json
import math
import re


def _id(value, width):
    return (isinstance(value, str) and re.fullmatch('[0-9a-f]{%d}' % width, value)
            and value != '0' * width)


def load_log_summary(path, *, trace_id=None, run_id=None, session_id=None):
    def failure(status, message):
        return dict(status=status, complete=False, partial=status == 'partial',
                    source=str(path), schema='capstan.log.v1', error=message)

    if not path.is_file():
        return None
    try:
        content = path.read_text(encoding='utf-8')
        if not content.endswith('\n'):
            raise ValueError('unterminated canonical log record')
        spans = {}
        active = set()
        active_children = {}
        identities = set()
        for line in content.splitlines():
            record = json.loads(line)
            if not isinstance(record, dict) or record.get('schema') != 'capstan.log.v1':
                raise ValueError('invalid canonical log schema')
            if record.get('category') != 'telemetry' or record.get('message') not in {
                    'span.started', 'span.finished'}:
                continue
            attrs = record.get('attributes')
            if not isinstance(attrs, list):
                raise ValueError('missing lifecycle attributes')
            values = {}
            for attr in attrs:
                if (not isinstance(attr, dict) or not isinstance(attr.get('key'), str)
                        or 'value' not in attr):
                    raise ValueError('invalid lifecycle attribute')
                key, value = attr['key'], attr['value']
                # Native start/end numeric attributes can repeat; identity cannot.
                if key in values and (key in {'span.name', 'parent.span_id', 'run.id',
                                             'session.id', 'outcome', 'cancelled'}):
                    raise ValueError('duplicate lifecycle identity or outcome')
                values[key] = value
            trace, span = record.get('trace_id'), record.get('span_id')
            run, session = values.get('run.id'), values.get('session.id')
            parent = values.get('parent.span_id')
            if (not _id(trace, 32) or not _id(span, 16) or not _id(run, 16)
                    or (parent is not None and not _id(parent, 16))
                    or not isinstance(values.get('span.name'), str)
                    or (session is not None and (not isinstance(session, str) or not session))
                    or record.get('session_id') != session):
                raise ValueError('invalid lifecycle identity')
            identity = (trace, run, session)
            if ((trace_id is not None and trace != trace_id)
                    or (run_id is not None and run != run_id)
                    or (session_id is not None and session != session_id)):
                continue
            identities.add(identity)
            key = (trace, span)
            pair = spans.setdefault(key, {})
            event = record['message']
            if event in pair:
                raise ValueError('duplicate lifecycle event')
            if event == 'span.finished' and 'span.started' not in pair:
                raise ValueError('finish without preceding start')
            parent_key = (trace, parent) if parent is not None else None
            if parent_key is not None and parent_key not in active:
                # Native persistence deliberately follows run completion, using
                # its retained root context. This is not permission for arbitrary
                # late children or children that outlive their live parent.
                parent_pair = spans.get(parent_key, {})
                parent_finish = parent_pair.get('span.finished')
                deferred_save = (
                    values['span.name'] == 'operation'
                    and values.get('operation') == 'session_save'
                    and parent_finish is not None
                    and parent_finish[0] == identity
                    and parent_finish[1] is None
                    and parent_finish[2]['span.name'] == 'agent.run'
                    and parent == run)
                if not deferred_save:
                    raise ValueError('child outside parent lifecycle')
            if event == 'span.started':
                active.add(key)
                if parent_key is not None:
                    active_children[parent_key] = active_children.get(parent_key, 0) + 1
            else:
                if active_children.get(key, 0):
                    raise ValueError('parent finished with active children')
                active.remove(key)
                if parent_key is not None:
                    active_children[parent_key] = active_children.get(parent_key, 0) - 1
            pair[event] = (identity, parent, values)
        if len(identities) != 1:
            raise ValueError('canonical log identity absent or ambiguous')
        if any(len(pair) != 2 for pair in spans.values()):
            return failure('partial', 'unfinished canonical lifecycle')
        roots = []
        for key, pair in spans.items():
            start = pair['span.started']
            finish = pair['span.finished']
            if start[:2] != finish[:2] or start[2]['span.name'] != finish[2]['span.name']:
                raise ValueError('lifecycle identity or parent changed')
            attrs = finish[2]
            if (attrs.get('outcome') not in {'success', 'error', 'cancelled'}
                    or not isinstance(attrs.get('cancelled'), bool)
                    or attrs['cancelled'] != (attrs['outcome'] == 'cancelled')
                    or 'outcome' in start[2] or 'cancelled' in start[2]):
                raise ValueError('invalid lifecycle outcome')
            parent = start[1]
            if parent is None:
                roots.append((key, attrs))
            else:
                seen = {key[1]}
                while parent is not None:
                    if parent in seen or (key[0], parent) not in spans:
                        raise ValueError('missing or cyclic parent')
                    seen.add(parent)
                    parent = spans[key[0], parent]['span.started'][1]
        if len(roots) != 1 or roots[0][1]['span.name'] != 'agent.run':
            raise ValueError('expected one agent.run root')
        (trace, span), attrs = roots[0]
        if span != attrs['run.id']:
            raise ValueError('root does not own run identity')
        correlation = dict(trace_id=trace, span_id=span, run_id=attrs['run.id'])
        if session_id := attrs.get('session.id'):
            correlation['session_id'] = session_id
        terminal = {'ok': attrs['outcome'] == 'success', 'outcome': attrs['outcome']}
        # Only adapt explicit producer-owned root measurements. Child spans and
        # log timestamps never supply missing totals.
        counts = {'request_count': 'model_requests', 'tool_count': 'tool_calls'}
        breakdown = {'model_ms', 'tool_ms', 'permission_wait_ms',
                     'subagent_wait_ms', 'unattributed_ms', 'overlap_ms'}
        for key in ('duration_ms', 'turns', *counts, *sorted(breakdown)):
            if key not in attrs:
                continue
            value = attrs[key]
            if (not isinstance(value, (int, float)) or isinstance(value, bool)
                    or not math.isfinite(value) or value < 0
                    or ((key == 'turns' or key in counts) and not isinstance(value, int))):
                raise ValueError('invalid root measurement')
            if key in counts:
                terminal.setdefault('counts', {})[counts[key]] = value
            elif key in breakdown:
                terminal.setdefault('breakdown', {})[key] = value
            else:
                terminal[key] = value
        # No intended process exit code or loss-free delivery is implied.
        return dict(status='complete', complete=True, partial=False, source=str(path),
                    schema='capstan.log.v1', correlation=correlation,
                    observed_events=2 * len(spans),
                    terminal=terminal)
    except (OSError, UnicodeError, ValueError, TypeError, KeyError, OverflowError):
        return failure('corrupt', 'invalid, ambiguous, or inconsistent canonical lifecycle')
