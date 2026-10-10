"""Offline provider fixture: tool evidence survives a CLI process restart."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/capstan').resolve())
requests = []
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass
    def do_POST(self):
        requests.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
        if len(requests) == 1:
            delta = {'tool_calls': [{'index': 0, 'id': 'persisted-call', 'type': 'function',
                     'function': {'name': 'shell', 'arguments': json.dumps({'command': 'printf evidence-42'})}}]}
        else:
            delta = {'content': 'Finished fixture.'}
        body = ('data: ' + json.dumps({'choices': [{'delta': delta}]}) + '\n\ndata: [DONE]\n\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

with tempfile.TemporaryDirectory(prefix='capstan-history-') as directory:
    root = Path(directory)
    config = root / '.config/capstan'
    config.mkdir(parents=True)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    (config / 'config.lua').write_text('''return {provider='fixture', providers={fixture={
        endpoint='http://127.0.0.1:%d/chat', model='fixture', context_limit=100000}},
        agent={completion_review=false, max_stream_retries=0}}''' % server.server_port)
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / '.config'), XDG_STATE_HOME=str(root / '.state'))
    for prompt in ('inspect', 'continue'):
        run = subprocess.run([binary, 'run', '--session-id', 'history', '--prompt', prompt,
                              '--no-mcp', '--no-wiki', '--yolo', '--json'],
                             cwd=root, env=env, capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, (run.stdout, run.stderr)
    assert len(requests) == 3, len(requests)
    messages = requests[2]['messages']
    assert any(m.get('tool_call_id') == 'persisted-call' and 'evidence-42' in m['content'] for m in messages)
    assert any(any(c['id'] == 'persisted-call' for c in m.get('tool_calls', [])) for m in messages)
    assert sum(m.get('content') == 'Finished fixture.' for m in messages) == 1
    server.shutdown()
print('CLI structured history survives process restart')
