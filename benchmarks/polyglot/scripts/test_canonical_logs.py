import copy
import json
from pathlib import Path
import tempfile
import unittest

from benchmarks.polyglot.scripts.run_eval import load_log_summary, reconcile_process_telemetry
from benchmarks.polyglot.scripts.analyze_traces import avg_telemetry, metric_telemetry_rows, validate_result_row


def records(trace='a' * 32):
    result = []
    for span, parent, name in [('b' * 16, None, 'agent.run'), ('c' * 16, 'b' * 16, 'agent.model')]:
        for event in ['span.started', 'span.finished']:
            attrs = {'span.name': name, 'run.id': 'b' * 16, 'session.id': 'd' * 32}
            if parent:
                attrs['parent.span_id'] = parent
            if event.endswith('finished'):
                attrs.update(outcome='success', cancelled=False)
            result.append(dict(schema='capstan.log.v1', category='telemetry', message=event,
                               trace_id=trace, span_id=span, session_id='d' * 32,
                               attributes=[dict(key=k, value=v) for k, v in attrs.items()]))
    # Proper nested lifecycle ordering.
    return [result[0], result[2], result[3], result[1]]


class CanonicalTests(unittest.TestCase):
    def load(self, rows, tail='\n', **selection):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'log.jsonl'
            path.write_text('\n'.join(json.dumps(r) for r in rows) + tail)
            return load_log_summary(path, **selection)

    def test_complete_without_invented_metrics(self):
        result = self.load(records())
        self.assertTrue(result['complete'])
        self.assertEqual(result['terminal'], {'ok': True, 'outcome': 'success'})
        self.assertEqual(metric_telemetry_rows([{'agent': {'telemetry': result}}]), [])
        self.assertTrue(reconcile_process_telemetry(result, timed_out=False, return_code=0)['complete'])
        for timeout, code in [(True, 0), (False, 1)]:
            self.assertEqual(reconcile_process_telemetry(result, timed_out=timeout, return_code=code)['status'], 'inconsistent')
        row = dict(status='passed', seconds=1, agent=dict(seconds=1, timed_out=False, return_code=0, telemetry=result))
        validate_result_row(row, Path('fixture'))
        self.assertTrue(row['agent']['telemetry']['complete'])

    def test_only_explicit_root_measurements(self):
        rows = records()
        rows[2]['attributes'].extend([
            dict(key='duration_ms', value=999), dict(key='turns', value=99)])
        rows[-1]['attributes'].extend([
            dict(key='duration_ms', value=12.5), dict(key='turns', value=2),
            dict(key='model_ms', value=10), dict(key='tool_calls', value=42)])
        result = self.load(rows)
        self.assertEqual(result['terminal'], {
            'ok': True, 'outcome': 'success', 'duration_ms': 12.5, 'turns': 2,
            'breakdown': {'model_ms': 10}})
        row = dict(status='passed', seconds=1, agent=dict(
            seconds=1, timed_out=False, return_code=0, telemetry=result))
        validate_result_row(row, Path('fixture'))
        self.assertEqual(avg_telemetry([row], ('duration_ms',)), 12.5)
        self.assertEqual(avg_telemetry([row], ('turns',)), 2)
        self.assertIsNone(avg_telemetry([row], ('counts', 'model_requests')))
        self.assertEqual(metric_telemetry_rows([row]), [])
        self.assertEqual(self.load(rows[:-1] + records()[-1:])['terminal'],
                         {'ok': True, 'outcome': 'success'})

    def test_producer_aggregates_and_old_missing_metrics(self):
        rows = records()
        fields = dict(request_count=2, tool_count=1, model_ms=10, tool_ms=3,
                      permission_wait_ms=1, subagent_wait_ms=0, unattributed_ms=2,
                      overlap_ms=0)
        rows[-1]['attributes'].extend(dict(key=k, value=v) for k, v in fields.items())
        summary = self.load(rows)
        terminal = summary['terminal']
        self.assertEqual(terminal['counts'], {'model_requests': 2, 'tool_calls': 1})
        row = {'agent': {'telemetry': summary}}
        self.assertEqual(metric_telemetry_rows([row]), [row])
        self.assertEqual(avg_telemetry([row], ('breakdown', 'model_ms')), 10)
        for key in fields:
            for bad in (-1, True, float('nan'), '2', 10**400):
                changed = copy.deepcopy(rows)
                next(a for a in changed[-1]['attributes'] if a['key'] == key)['value'] = bad
                self.assertEqual(self.load(changed)['status'], 'corrupt')
        for key in ('request_count', 'tool_count'):
            changed = copy.deepcopy(rows)
            next(a for a in changed[-1]['attributes'] if a['key'] == key)['value'] = 1.5
            self.assertEqual(self.load(changed)['status'], 'corrupt')
        self.assertNotIn('counts', self.load(records())['terminal'])

    def test_invalid_root_measurements(self):
        for key, values in [('duration_ms', [-1, True, '12', float('inf'), float('nan')]),
                            ('turns', [-1, True, '2', 1.5, 10**400]),
                            ('duration_ms', [10**400])]:
            for value in values:
                with self.subTest(key=key, value=value):
                    rows = records()
                    rows[-1]['attributes'].append(dict(key=key, value=value))
                    self.assertEqual(self.load(rows)['status'], 'corrupt')
        rows = records()
        rows[-1]['attributes'].extend([
            dict(key='duration_ms', value=0), dict(key='turns', value=0)])
        self.assertEqual(self.load(rows)['terminal']['turns'], 0)
        self.assertEqual(self.load(rows)['terminal']['duration_ms'], 0)

    def test_children_require_live_parent(self):
        rows = records()
        for order in [(1, 0, 2, 3), (0, 3, 1, 2), (0, 1, 3, 2),
                      (1, 2, 0, 3), (0, 1, 3)]:
            with self.subTest(order=order):
                self.assertEqual(self.load([rows[i] for i in order])['status'], 'corrupt')

    def test_deferred_session_save_requires_completed_run_root(self):
        root_start, save_start, save_finish, root_finish = records()
        for row in (save_start, save_finish):
            next(a for a in row['attributes'] if a['key'] == 'span.name')['value'] = 'operation'
            row['attributes'].append(dict(key='operation', value='session_save'))
        rows = [root_start, root_finish, save_start, save_finish]
        self.assertTrue(self.load(rows)['complete'])
        self.assertEqual(self.load(rows[:-1])['status'], 'partial')
        # A save cannot begin before its parent or straddle root completion.
        for order in ([save_start, root_start, root_finish, save_finish],
                      [root_start, save_start, root_finish, save_finish]):
            self.assertEqual(self.load(order)['status'], 'corrupt')
        for index in (2, 3):
            changed = copy.deepcopy(rows)
            next(a for a in changed[index]['attributes']
                 if a['key'] == 'operation')['value'] = 'other'
            self.assertEqual(self.load(changed)['status'], 'corrupt')
        changed = copy.deepcopy(rows)
        for row in changed[2:]:
            next(a for a in row['attributes'] if a['key'] == 'run.id')['value'] = 'f' * 16
        self.assertEqual(self.load(changed)['status'], 'corrupt')
        # A completed model span is not a deferred persistence owner.
        root_start, child_start, child_finish, root_finish = records()
        nested_save = copy.deepcopy([save_start, save_finish])
        for row in nested_save:
            row['span_id'] = 'e' * 16
            next(a for a in row['attributes']
                 if a['key'] == 'parent.span_id')['value'] = 'c' * 16
        self.assertEqual(self.load([root_start, child_start, child_finish,
                                    *nested_save, root_finish])['status'], 'corrupt')

    def test_parallel_siblings_need_not_finish_in_stack_order(self):
        root_start, child_start, child_finish, root_finish = records()
        sibling_start, sibling_finish = copy.deepcopy([child_start, child_finish])
        for row in (sibling_start, sibling_finish):
            row['span_id'] = 'e' * 16
        for finishes in ([child_finish, sibling_finish], [sibling_finish, child_finish]):
            rows = [root_start, child_start, sibling_start, *finishes, root_finish]
            self.assertTrue(self.load(rows)['complete'])
        self.assertEqual(self.load([root_start, child_start, sibling_start])['status'], 'partial')

    def test_shared_identity(self):
        rows = records() + records('e' * 32)
        self.assertEqual(self.load(rows)['status'], 'corrupt')
        self.assertTrue(self.load(rows, trace_id='e' * 32)['complete'])
        self.assertEqual(self.load(rows, session_id='d' * 32)['status'], 'corrupt')
        self.assertEqual(self.load(rows, trace_id='f' * 32)['status'], 'corrupt')

    def test_corruption_and_incomplete(self):
        self.assertEqual(self.load(records()[:-1])['status'], 'partial')
        self.assertEqual(self.load(records(), tail='')['status'], 'corrupt')
        self.assertEqual(self.load(records() + records()[:1])['status'], 'corrupt')
        self.assertEqual(self.load(records()[1:])['status'], 'corrupt')
        for key, value in [('parent.span_id', 'f' * 16), ('run.id', 'f' * 16),
                           ('span.name', 'tool'), ('outcome', 'unknown'), ('cancelled', True)]:
            rows = copy.deepcopy(records())
            attrs = rows[2]['attributes']
            attrs[:] = [a for a in attrs if a['key'] != key]
            attrs.append(dict(key=key, value=value))
            self.assertEqual(self.load(rows)['status'], 'corrupt', key)
        rows = records()
        for row in rows[1:3]:
            next(a for a in row['attributes'] if a['key'] == 'parent.span_id')['value'] = 'c' * 16
        self.assertEqual(self.load(rows)['status'], 'corrupt')


if __name__ == '__main__':
    unittest.main()
