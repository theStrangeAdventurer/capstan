import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('probe', Path(__file__).resolve().parents[1] /
    'benchmarks/polyglot/scripts/probe_openrouter.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class ProbeTests(unittest.TestCase):
    def test_fragmented_sse_and_combined_usage(self):
        payload = {'id': 'gen-test', 'model': 'model', 'provider': 'backend',
                   'choices': [{'delta': {'content': 'x'}, 'finish_reason': 'stop'}],
                   'usage': {'completion_tokens': 120, 'completion_tokens_details': {'reasoning_tokens': 80}}}
        wire = ('data: ' + json.dumps(payload) + '\r\n\r\ndata: [DONE]\r\n\r\n').encode()
        class Response:
            chunks = iter(bytes([b]) for b in wire)
            def read1(self, _):
                return next(self.chunks, b'')
        times = iter([2, 5])
        result = probe.read_stream(Response(), 1, lambda: next(times))
        self.assertEqual(result['first_output_seconds'], 1)
        self.assertEqual(result['upstream_provider'], 'backend')
        self.assertEqual(result['response_id'], 'gen-test')
        self.assertEqual(result['completion_tokens_per_second_after_first_output'], 40)
        self.assertNotIn('error', result)

    def test_truncated_stream_is_not_success(self):
        class Response:
            def read1(self, _):
                return b''
        self.assertEqual(probe.read_stream(Response(), 0, lambda: 1)['error'], 'incomplete_stream')

    def test_only_routing_changes_body(self):
        automatic = probe.request_body('same-model', None)
        fast = probe.request_body('same-model', {'sort': 'throughput'})
        self.assertEqual(fast.pop('provider'), {'sort': 'throughput'})
        self.assertEqual(automatic, fast)
        self.assertEqual(automatic['max_tokens'], 32000)
        self.assertEqual(automatic['reasoning'], {'effort': 'medium'})


if __name__ == '__main__':
    unittest.main()
