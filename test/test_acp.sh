#!/bin/sh
set -eu

binary=${1:-build/capstan}
case "$binary" in
  /*) ;;
  *) binary="$(pwd)/$binary" ;;
esac
tmpdir=$(mktemp -d /tmp/capstan-acp-test.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT INT TERM
mkdir -p "$tmpdir/home/.config/capstan/plugins" "$tmpdir/workspace" "$tmpdir/outside"
cat >"$tmpdir/home/.config/capstan/config.lua" <<'LUA'
return {
  provider = "acp_capture",
  providers = {
    acp_capture = {
      endpoint = "http://127.0.0.1:1/v1/chat/completions",
      model = "capture",
      context_limit = 4096,
    },
  },
  agent = {completion_review = false, max_stream_retries = 0},
  hooks = {
    before_request = function(ctx)
      local capture = os.getenv("ACP_CAPTURE_PATH")
      if not capture then return ctx end
      local messages = ctx.request.messages or {}
      local last = messages[#messages] or {}
      local content = last.content
      local image = type(content) == "table" and content[2] or nil
      local image_url = image and image.image_url or nil
      if last.role ~= "user" or type(content) ~= "table" or
          content[1].type ~= "text" or content[1].text ~= "Inspect this image" or
          not image or image.type ~= "image_url" or
          not image_url or image_url.detail ~= "auto" or
          image_url.url ~= "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII=" then
        error("ACP image was not converted to canonical provider content")
      end
      local file = assert(io.open(capture, "w"))
      file:write("ok\n")
      file:close()
      return ctx
    end,
  },
}
LUA
cat >"$tmpdir/home/.config/capstan/plugins/noisy.lua" <<'LUA'
print("plugin output must not corrupt ACP stdout")
return {id = "noisy", command = "/noisy", description = "Transport isolation test", handler = function() return "ok" end}
LUA

request_file="$tmpdir/requests.ndjson"
cat >"$request_file" <<EOF
{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"clientCapabilities":{}}}
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":"$tmpdir/workspace","mcpServers":[]}}
{"jsonrpc":"2.0","id":3,"method":"session/new","params":{"cwd":"$tmpdir/outside","mcpServers":[]}}
{"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{}}}
{"jsonrpc":"2.0","id":5,"method":"session/new","params":{"cwd":"$tmpdir/workspace","mcpServers":[{"name":"client-server","command":"example-mcp"}]}}
{"jsonrpc":"2.0","method":"session/cancel","params":{"sessionId":"unknown"}}
EOF

(cd "$tmpdir/workspace" && HOME="$tmpdir/home" "$binary" acp <"$request_file" >"$tmpdir/responses.ndjson")

python3 - "$tmpdir/responses.ndjson" <<'PY'
import json
import sys

messages = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
by_id = {message.get("id"): message for message in messages if "id" in message}
assert by_id[0]["error"]["code"] == -32602
assert by_id[1]["result"]["protocolVersion"] == 1
assert by_id[1]["result"]["agentInfo"]["name"] == "capstan"
assert by_id[1]["result"]["agentCapabilities"]["promptCapabilities"]["image"] is True
assert by_id[1]["result"]["agentCapabilities"]["mcpCapabilities"] == {"http": True, "sse": False}
session = by_id[2]["result"]
assert session["sessionId"].startswith("capstan-")
assert {item["id"] for item in session["configOptions"]} == {"model", "mode", "effort"}
assert session["modes"]["currentModeId"] == "implement"
assert by_id[1]["result"]["agentCapabilities"]["sessionCapabilities"]["close"] == {}
assert by_id[3]["error"]["code"] == -32602
assert "bound to workspace" in by_id[3]["error"]["message"]
assert by_id[4]["error"]["code"] == -32600
assert "more than once" in by_id[4]["error"]["message"]
assert by_id[5]["error"]["code"] == -32602
assert "absolute path" in by_id[5]["error"]["message"]
assert not any(message.get("id", "missing") is None for message in messages)
updates = [m for m in messages if m.get("method") == "session/update"]
assert any(m["params"]["update"]["sessionUpdate"] == "available_commands_update" for m in updates)
PY

ACP_CAPTURE_PATH="$tmpdir/image-captured" python3 - "$binary" "$tmpdir/home" "$tmpdir/workspace" <<'PY'
import json
import os
import subprocess
import sys

binary, home, workspace = sys.argv[1:]
env = os.environ.copy()
env["HOME"] = home
proc = subprocess.Popen(
    [binary, "acp"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    env=env,
    cwd=workspace,
)

def send(message):
    proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    proc.stdin.flush()

seen = []

def response(request_id):
    while True:
        line = proc.stdout.readline()
        if not line:
            raise AssertionError(f"ACP exited before response {request_id}: {proc.stderr.read()}")
        message = json.loads(line)
        seen.append(message)
        if message.get("id") == request_id:
            return message

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "protocolVersion": 1, "clientCapabilities": {}}})
assert response(1)["result"]["agentCapabilities"]["promptCapabilities"]["image"] is True
send({"jsonrpc": "2.0", "id": 2, "method": "session/new", "params": {
    "cwd": workspace, "mcpServers": []}})
session_id = response(2)["result"]["sessionId"]

send({"jsonrpc": "2.0", "id": 3, "method": "session/prompt", "params": {
    "sessionId": session_id,
    "prompt": [{"type": "text", "text": "/plan"}],
}})
assert response(3)["result"]["stopReason"] == "end_turn"
assert any(
    message.get("method") == "session/update" and
    message["params"]["update"].get("sessionUpdate") == "current_mode_update" and
    message["params"]["update"].get("modeId") == "plan"
    for message in seen
)

send({"jsonrpc": "2.0", "id": 4, "method": "session/prompt", "params": {
    "sessionId": session_id,
    "prompt": [{"type": "image", "mimeType": "image/png", "data": "not-base64"}],
}})
invalid = response(4)
assert invalid["error"]["code"] == -32602
assert "base64" in invalid["error"]["message"]

send({"jsonrpc": "2.0", "id": 5, "method": "session/prompt", "params": {
    "sessionId": session_id,
    "prompt": [{"type": "image", "mimeType": "image/png", "data": "aGVsbG8="}],
}})
assert response(5)["error"]["code"] == -32602

send({"jsonrpc": "2.0", "id": 6, "method": "session/prompt", "params": {
    "sessionId": session_id,
    "prompt": [{"type": "image", "mimeType": "image/jpeg",
                "data": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII="}],
}})
assert response(6)["error"]["code"] == -32602

send({"jsonrpc": "2.0", "id": 7, "method": "session/prompt", "params": {
    "sessionId": session_id,
    "prompt": [
        {"type": "text", "text": "Inspect this image"},
        {"type": "image", "mimeType": "image/png",
         "data": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII="},
    ],
}})
proc.stdin.close()
assert proc.wait(timeout=10) == 0, proc.stderr.read()
PY

test "$(cat "$tmpdir/image-captured")" = "ok"

python3 - "$binary" "$tmpdir/home" "$tmpdir/workspace" "$tmpdir" <<'PY'
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary, home, workspace, tmpdir = sys.argv[1:]
mcp_events = os.path.join(tmpdir, "mcp-events")
mcp_script = os.path.join(tmpdir, "mcp-server.py")
with open(mcp_script, "w", encoding="utf-8") as file:
    file.write(r'''import json
import os
import sys

marker = os.environ["MCP_TEST_EVENTS"]
for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    with open(marker, "a", encoding="utf-8") as events:
        events.write(str(method) + "\n")
    if "id" not in request:
        continue
    if method == "initialize":
        result = {
            "protocolVersion": "2025-06-18",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "acp-test", "version": "1"},
        }
    elif method == "tools/list":
        result = {"tools": [{
            "name": "demo",
            "description": "ACP configured MCP test tool",
            "inputSchema": {"type": "object", "properties": {}},
        }]}
    else:
        result = {}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
''')

provider_requests = []

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        body = json.dumps({"data": [{"id": "mcp-model"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        provider_requests.append(json.loads(self.rfile.read(length)))
        event = {"choices": [{"delta": {"content": "ok"}, "finish_reason": "stop"}]}
        body = ("data: " + json.dumps(event) + "\n\ndata: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
port = server.server_address[1]
config = f'''return {{
  provider = "mcp_capture",
  providers = {{
    mcp_capture = {{
      endpoint = "http://127.0.0.1:{port}/v1/chat/completions",
      model = "mcp-model",
      context_limit = 4096,
    }},
  }},
  agent = {{completion_review = false, max_stream_retries = 0}},
  mcp = {{
    enabled = true,
    servers = {{{{
      name = "acp_test",
      command = {json.dumps(sys.executable)},
      args = {{{json.dumps(mcp_script)}}},
      env = {{MCP_TEST_EVENTS = {json.dumps(mcp_events)}}},
    }}}},
  }},
}}\n'''
with open(os.path.join(home, ".config", "capstan", "config.lua"), "w", encoding="utf-8") as file:
    file.write(config)

env = os.environ.copy()
env["HOME"] = home
proc = subprocess.Popen(
    [binary, "acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, env=env, cwd=workspace,
)

def send(message):
    proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    proc.stdin.flush()

def response(request_id):
    while True:
        line = proc.stdout.readline()
        if not line:
            raise AssertionError(f"ACP exited before response {request_id}: {proc.stderr.read()}")
        message = json.loads(line)
        if message.get("id") == request_id:
            return message

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "protocolVersion": 1, "clientCapabilities": {}}})
assert response(1)["result"]["protocolVersion"] == 1
send({"jsonrpc": "2.0", "id": 2, "method": "session/new", "params": {
    "cwd": workspace, "mcpServers": []}})
session_id = response(2)["result"]["sessionId"]

for request_id in (3, 4):
    send({"jsonrpc": "2.0", "id": request_id, "method": "session/prompt", "params": {
        "sessionId": session_id,
        "prompt": [{"type": "text", "text": f"MCP load check {request_id}"}],
    }})
    assert response(request_id)["result"]["stopReason"] == "end_turn"

proc.stdin.close()
assert proc.wait(timeout=10) == 0, proc.stderr.read()
server.shutdown()
assert len(provider_requests) == 2
first_tools = {tool["function"]["name"] for tool in provider_requests[0].get("tools", [])}
second_tools = {tool["function"]["name"] for tool in provider_requests[1].get("tools", [])}
assert "mcp__acp_test__demo" in first_tools, (first_tools, second_tools)
assert "mcp__acp_test__demo" in second_tools, (first_tools, second_tools)
with open(mcp_events, encoding="utf-8") as file:
    events = file.read().splitlines()
assert "initialize" in events
assert "tools/list" in events
PY

python3 - "$binary" "$tmpdir/home" "$tmpdir/workspace" "$tmpdir" <<'PY'
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary, home, workspace, tmpdir = sys.argv[1:]
script = os.path.join(tmpdir, "scoped-mcp.py")
with open(script, "w", encoding="utf-8") as file:
    file.write(r'''import json
import os
import signal
import sys

marker = os.environ["MCP_CLOSE_MARKER"]
tool_name = os.environ["MCP_TOOL_NAME"]
call_marker = os.environ["MCP_CALL_MARKER"]
with open(os.environ["MCP_PID_MARKER"], "w", encoding="utf-8") as output:
    output.write(str(os.getpid()))
def close(*_args):
    with open(marker, "w", encoding="utf-8") as output:
        output.write("closed\n")
    raise SystemExit(0)
signal.signal(signal.SIGTERM, close)
for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    if request.get("method") == "initialize":
        result = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}},
                  "serverInfo": {"name": tool_name, "version": "1"}}
    elif request.get("method") == "tools/list":
        result = {"tools": [{"name": tool_name, "description": tool_name,
                              "inputSchema": {"type": "object", "properties": {}}}]}
    elif request.get("method") == "tools/call":
        with open(call_marker, "a", encoding="utf-8") as output:
            output.write(request["params"]["name"] + "\n")
        result = {"content": [{"type": "text", "text": tool_name + " called"}]}
    else:
        result = {}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
''')

http_mcp_requests = []
http_mcp_deleted = []
class McpHandler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        http_mcp_requests.append((request, self.headers.get("X-Scope-Test")))
        if "id" not in request:
            self.send_response(202)
            self.end_headers()
            return
        if request.get("method") == "initialize":
            result = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}},
                      "serverInfo": {"name": "http-scope", "version": "1"}}
        else:
            result = {"tools": [{"name": "gamma", "description": "gamma",
                                  "inputSchema": {"type": "object", "properties": {}}}]}
        body = json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Mcp-Session-Id", "gamma-session")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_DELETE(self):
        http_mcp_deleted.append(self.headers.get("Mcp-Session-Id"))
        self.send_response(204)
        self.end_headers()

mcp_http_server = ThreadingHTTPServer(("127.0.0.1", 0), McpHandler)
threading.Thread(target=mcp_http_server.serve_forever, daemon=True).start()
mcp_http_port = mcp_http_server.server_address[1]

provider_requests = []
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass
    def do_GET(self):
        body = json.dumps({"data": [{"id": "scoped-mcp-model"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        provider_requests.append(request)
        last = request.get("messages", [{}])[-1]
        if last.get("role") == "user" and last.get("content") == "invoke alpha":
            events = [
                {"choices": [{"delta": {"tool_calls": [{"index": 0,
                    "id": "call_alpha", "type": "function", "function": {
                    "name": "mcp__session_server__alpha", "arguments": "{}"}}]},
                    "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
            ]
        else:
            events = [{"choices": [{"delta": {"content": "ok"}, "finish_reason": "stop"}]}]
        payload = "".join("data: " + json.dumps(event) + "\n\n" for event in events)
        body = (payload + "data: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
port = server.server_address[1]
config = f'''return {{
  provider = "scoped_mcp_capture",
  providers = {{scoped_mcp_capture = {{
    endpoint = "http://127.0.0.1:{port}/v1/chat/completions",
    model = "scoped-mcp-model", context_limit = 4096,
  }}}},
  agent = {{completion_review = false, max_stream_retries = 0}},
}}\n'''
with open(os.path.join(home, ".config", "capstan", "config.lua"), "w", encoding="utf-8") as file:
    file.write(config)

env = os.environ.copy()
env["HOME"] = home
proc = subprocess.Popen([binary, "acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env=env, cwd=workspace)
def send(message):
    proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    proc.stdin.flush()
def response(request_id):
    while True:
        line = proc.stdout.readline()
        if not line:
            raise AssertionError(f"ACP exited before response {request_id}: {proc.stderr.read()}")
        message = json.loads(line)
        if message.get("id") == request_id:
            return message

def descriptor(tool_name):
    return {"name": "session_server", "command": sys.executable,
            "args": [script], "env": [
                {"name": "MCP_TOOL_NAME", "value": tool_name},
                {"name": "MCP_CLOSE_MARKER", "value": os.path.join(tmpdir, tool_name + ".closed")},
                {"name": "MCP_PID_MARKER", "value": os.path.join(tmpdir, tool_name + ".pid")},
                {"name": "MCP_CALL_MARKER", "value": os.path.join(tmpdir, tool_name + ".calls")},
            ]}

def tool_names(request):
    return {tool["function"]["name"] for tool in request.get("tools", [])}

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "protocolVersion": 1, "clientCapabilities": {}}})
assert response(1)["result"]["agentCapabilities"]["mcpCapabilities"]["http"] is True
send({"jsonrpc": "2.0", "id": 2, "method": "session/new", "params": {
    "cwd": workspace, "mcpServers": [descriptor("alpha")]}})
alpha_session = response(2)["result"]["sessionId"]
send({"jsonrpc": "2.0", "id": 3, "method": "session/new", "params": {
    "cwd": workspace, "mcpServers": [descriptor("beta")]}})
beta_session = response(3)["result"]["sessionId"]
send({"jsonrpc": "2.0", "id": 4, "method": "session/new", "params": {
    "cwd": workspace, "mcpServers": [{"type": "http", "name": "http_session",
        "url": f"http://127.0.0.1:{mcp_http_port}/mcp",
        "headers": [{"name": "X-Scope-Test", "value": "gamma-header"}]}]}})
gamma_session = response(4)["result"]["sessionId"]

for request_id, session_id in ((5, alpha_session), (6, beta_session), (7, gamma_session)):
    send({"jsonrpc": "2.0", "id": request_id, "method": "session/prompt", "params": {
        "sessionId": session_id, "prompt": [{"type": "text", "text": "scope check"}]}})
    assert response(request_id)["result"]["stopReason"] == "end_turn"

assert "mcp__session_server__alpha" in tool_names(provider_requests[0])
assert "mcp__session_server__beta" not in tool_names(provider_requests[0])
assert "mcp__http_session__gamma" not in tool_names(provider_requests[0])
assert "mcp__session_server__beta" in tool_names(provider_requests[1])
assert "mcp__session_server__alpha" not in tool_names(provider_requests[1])
assert "mcp__http_session__gamma" not in tool_names(provider_requests[1])
assert "mcp__http_session__gamma" in tool_names(provider_requests[2])
assert "mcp__session_server__alpha" not in tool_names(provider_requests[2])
assert "mcp__session_server__beta" not in tool_names(provider_requests[2])
assert [request[0]["method"] for request in http_mcp_requests] == [
    "initialize", "notifications/initialized", "tools/list"]
assert all(request[1] == "gamma-header" for request in http_mcp_requests)

send({"jsonrpc": "2.0", "id": 8, "method": "session/prompt", "params": {
    "sessionId": alpha_session, "prompt": [{"type": "text", "text": "invoke alpha"}]}})
while True:
    line = proc.stdout.readline()
    if not line:
        raise AssertionError(f"ACP exited during scoped tool call: {proc.stderr.read()}")
    message = json.loads(line)
    if message.get("method") == "session/request_permission":
        send({"jsonrpc": "2.0", "id": message["id"], "result": {
            "outcome": {"outcome": "selected", "optionId": "allow-once"}}})
    elif message.get("id") == 8:
        assert message["result"]["stopReason"] == "end_turn"
        break
assert open(os.path.join(tmpdir, "alpha.calls"), encoding="utf-8").read().splitlines() == ["alpha"]
assert not os.path.exists(os.path.join(tmpdir, "beta.calls"))

with open(os.path.join(tmpdir, "alpha.pid"), encoding="utf-8") as file:
    alpha_pid = int(file.read())
send({"jsonrpc": "2.0", "id": 9, "method": "session/close", "params": {
    "sessionId": alpha_session}})
assert response(9)["result"] == {}
try:
    os.kill(alpha_pid, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError("session/close left its MCP process alive")

send({"jsonrpc": "2.0", "id": 10, "method": "session/prompt", "params": {
    "sessionId": beta_session, "prompt": [{"type": "text", "text": "still isolated"}]}})
assert response(10)["result"]["stopReason"] == "end_turn"
assert "mcp__session_server__beta" in tool_names(provider_requests[5])
assert "mcp__session_server__alpha" not in tool_names(provider_requests[5])
assert "mcp__http_session__gamma" not in tool_names(provider_requests[5])

send({"jsonrpc": "2.0", "id": 11, "method": "session/close", "params": {
    "sessionId": gamma_session}})
assert response(11)["result"] == {}
assert http_mcp_deleted == ["gamma-session"]
with open(os.path.join(tmpdir, "beta.pid"), encoding="utf-8") as file:
    beta_pid = int(file.read())
proc.stdin.close()
assert proc.wait(timeout=10) == 0, proc.stderr.read()
try:
    os.kill(beta_pid, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError("ACP disconnect left a session MCP process alive")
server.shutdown()
mcp_http_server.shutdown()
PY

python3 - "$binary" "$tmpdir/home" "$tmpdir/workspace" <<'PY'
import json
import os
import queue
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary, home, workspace = sys.argv[1:]
requests = []

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        body = json.dumps({"data": [{"id": "permission-model"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        requests.append(json.loads(self.rfile.read(length)))
        if len(requests) in (1, 3, 5):
            events = [
                {"choices": [{"delta": {"tool_calls": [{
                    "index": 0, "id": f"call_permission_{len(requests)}", "type": "function",
                    "function": {"name": "shell", "arguments":
                        json.dumps({"command": "printf permission-ok"})},
                }]}, "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
            ]
        else:
            events = [
                {"choices": [{"delta": {"content": "permission completed"}, "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ]
        payload = "".join("data: " + json.dumps(event) + "\n\n" for event in events) + "data: [DONE]\n\n"
        body = payload.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
port = server.server_address[1]
config = f'''return {{
  provider = "permission_capture",
  providers = {{
    permission_capture = {{
      endpoint = "http://127.0.0.1:{port}/v1/chat/completions",
      model = "permission-model",
      context_limit = 4096,
    }},
  }},
  agent = {{completion_review = false, max_stream_retries = 0}},
}}\n'''
with open(os.path.join(home, ".config", "capstan", "config.lua"), "w", encoding="utf-8") as file:
    file.write(config)

env = os.environ.copy()
env["HOME"] = home
proc = subprocess.Popen(
    [binary, "acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, env=env, cwd=workspace,
)
stdout_messages = queue.Queue()
transcript = []
threading.Thread(
    target=lambda: [stdout_messages.put(line) for line in proc.stdout],
    daemon=True,
).start()

def send(message):
    proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    proc.stdin.flush()

def receive():
    try:
        line = stdout_messages.get(timeout=10)
    except queue.Empty:
        proc.kill()
        _, stderr = proc.communicate(timeout=2)
        raise AssertionError(
            f"timed out waiting for ACP message; stderr={stderr}; requests={requests}; transcript={transcript}")
    message = json.loads(line)
    transcript.append(message)
    return message

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "protocolVersion": 1, "clientCapabilities": {}}})
assert receive().get("id") == 1
session_id = None
permission = None
permission_requests = 0
tool_start = None
tool_done = None
send({"jsonrpc": "2.0", "id": 2, "method": "session/new", "params": {
    "cwd": workspace, "mcpServers": []}})
while session_id is None:
    message = receive()
    if message.get("id") == 2:
        session_id = message["result"]["sessionId"]

send({"jsonrpc": "2.0", "id": 3, "method": "session/prompt", "params": {
    "sessionId": session_id,
    "prompt": [{"type": "text", "text": "Run the permission test"}],
}})
while True:
    message = receive()
    if message.get("method") == "session/request_permission":
        permission = message
        permission_requests += 1
        send({"jsonrpc": "2.0", "id": message["id"], "result": {
            "outcome": {"outcome": "selected", "optionId": "allow-always"}}})
    elif message.get("method") == "session/update":
        update = message["params"]["update"]
        if update.get("sessionUpdate") == "tool_call":
            tool_start = update
        elif update.get("sessionUpdate") == "tool_call_update":
            tool_done = update
    elif message.get("id") == 3:
        assert message["result"]["stopReason"] == "end_turn"
        break

assert permission is not None
assert {item["optionId"] for item in permission["params"]["options"]} == {
    "allow-once", "allow-always", "reject-once"}
assert permission["params"]["toolCall"]["kind"] == "execute"
assert tool_start["kind"] == "execute"
assert tool_start["rawInput"]["command"] == "printf permission-ok"
assert tool_done["status"] == "completed"
assert "permission-ok" in tool_done["rawOutput"]["output"]
assert permission_requests == 1
assert len(requests) == 2

send({"jsonrpc": "2.0", "id": 4, "method": "session/prompt", "params": {
    "sessionId": session_id,
    "prompt": [{"type": "text", "text": "Run the permission test again"}],
}})
while True:
    message = receive()
    if message.get("method") == "session/request_permission":
        raise AssertionError("allow-always prompted again in the same ACP session")
    if message.get("id") == 4:
        assert message["result"]["stopReason"] == "end_turn"
        break

assert permission_requests == 1
assert len(requests) == 4

# A fresh session has no grant. Cancel while its permission request is pending;
# the run must resolve as cancelled and must not start a continuation request.
send({"jsonrpc": "2.0", "id": 5, "method": "session/new", "params": {
    "cwd": workspace, "mcpServers": []}})
cancel_session_id = None
while cancel_session_id is None:
    message = receive()
    if message.get("id") == 5:
        cancel_session_id = message["result"]["sessionId"]
send({"jsonrpc": "2.0", "id": 6, "method": "session/prompt", "params": {
    "sessionId": cancel_session_id,
    "prompt": [{"type": "text", "text": "Cancel this permission test"}],
}})
cancelled = False
new_during_run_rejected = False
while not cancelled:
    message = receive()
    if message.get("method") == "session/request_permission":
        send({"jsonrpc": "2.0", "id": 7, "method": "session/new", "params": {
            "cwd": workspace, "mcpServers": []}})
        send({"jsonrpc": "2.0", "method": "session/cancel", "params": {
            "sessionId": cancel_session_id}})
    elif message.get("id") == 7:
        assert message["error"]["code"] == -32000
        new_during_run_rejected = True
    elif message.get("id") == 6:
        assert message["result"]["stopReason"] == "cancelled"
        cancelled = True
assert new_during_run_rejected
# Give any erroneous recursive continuation enough time to reach the provider.
time.sleep(0.2)
assert len(requests) == 5, requests
assert not os.path.exists(os.path.join(home, ".local", "state", "capstan", "permissions.lua"))
proc.stdin.close()
assert proc.wait(timeout=10) == 0, proc.stderr.read()
server.shutdown()
PY
