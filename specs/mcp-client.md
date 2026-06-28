# MCP Client

Model Context Protocol (MCP) client support for capstan. Capstan acts as an
MCP **client** that connects to external MCP **servers** over stdio or
Streamable HTTP, discovers their tools, and exposes them to the LLM alongside
built-in plugin tools.

## Behavior

### Transport: stdio

- Capstan spawns each configured MCP server as a subprocess.
- Communication uses **newline-delimited JSON (NDJSON)**: each line on
  stdin/stdout is one complete JSON-RPC 2.0 message (no `Content-Length`
  framing).
- stderr from the server is redirected to `/dev/null`; it is not captured or
  parsed.

### Transport: Streamable HTTP

- Capstan connects to a configured remote MCP endpoint with JSON-RPC over HTTP.
- Requests are sent with `Content-Type: application/json`,
  `Accept: application/json, text/event-stream`, and `MCP-Protocol-Version`.
- If the server returns `Mcp-Session-Id`, Capstan stores it and sends it on
  later requests to the same server.
- HTTP responses may be JSON-RPC JSON or a finite `text/event-stream` response;
  Capstan extracts the JSON-RPC response matching the original request id.

### Lifecycle

1. **Connect** — either `fork+exec` the stdio server command or prepare the
   remote HTTP endpoint.
2. **Initialize** — send `initialize` request with `protocolVersion`,
   `clientInfo`, and `capabilities`. Wait for response.
3. **Initialized notification** — send `notifications/initialized`.
4. **Tools discovery** — send `tools/list`, cache the returned tool
  definitions.
5. **Ready** — tools are merged into the LLM tool list on every request.
6. **Shutdown** — close stdin, send SIGTERM, wait, SIGKILL if needed.

### Tool routing

- MCP tool names are exposed with a provider-safe prefix: e.g.
  `browser_navigate` from server `browser` becomes
  `mcp__browser__browser_navigate`.
- This prevents name collisions between MCP servers and built-in plugins.
- The names only use letters, numbers, `_`, and `-` because some providers
  reject dots in function names (`^[a-zA-Z0-9_-]+$`).
- The LLM sees the exposed name; when it calls the tool, capstan maps it back
  to the original MCP server/tool pair and calls `tools/call`.

### Tool call flow

```
LLM calls "mcp__browser__browser_navigate"
  → tools.lua:call_plugin_tool()
    → mcp.call("mcp__browser__browser_navigate", {url="..."})
      → fill missing arguments from tool schema defaults
      → MCP transport: send tools/call request
      → read response (blocking with timeout)
      → extract content[1].text from result
    → return text to LLM as tool result
```

### Permissions

- Each MCP tool is checked via `permit.check("mcp", tool_name)`.
- Default: `ask` — user sees a permission popup.
- Users can pre-grant via config: `{tool="mcp", pattern="https://example.com/*", allow=true}`.

### Error handling

- Server crash → log error, mark server as `disconnected`, tools removed from
  the LLM list.
- Tool call timeout (default 30s) → return error string to LLM.
- JSON-RPC error response → return error message to LLM.
- Tool execution error (`isError: true` in result) → return content text with
  error indication.
- Tool schemas can mark a property as required while also providing a default
  (for example Playwright's screenshot `type = "png"`). Before `tools/call`,
  capstan fills missing arguments from schema defaults to avoid avoidable MCP
  validation errors.
- `mcp.recv()` Lua/C errors are caught and converted into normal tool failures,
  not stream callback crashes.
- A timeout or recv failure during `tools/call` marks that MCP server as
  `failed` and kills the subprocess. The user can restart it with
  `/mcp restart <server>`.
- Relative artifact links returned by MCP tools, such as Playwright screenshot
  paths like `./page.png`, are rewritten against `capstan.workdir` before they
  are sent back to the model. This makes final answers point to the real file
  path instead of an ambiguous server-relative path.

## Configuration

In `~/.config/capstan/config.lua`:

```lua
capstan.config = {
  -- ...existing fields...

  mcp = {
    enabled = true,
    servers = {
      {
        name = "browser",
        enabled = true,
        transport = "stdio",
        command = "npx",
        args = {"-y", "@playwright/mcp", "--headless"},
        env = {},                    -- optional extra env vars
        timeout = 30000,             -- per-call timeout in ms (default 30000)
      },
      {
        name = "tracker",
        enabled = true,
        transport = "http",          -- or "streamable_http"
        url = "https://example.com/mcp",
        headers = {
          Authorization = "Bearer ...",
        },
        timeout = 30000,
      },
    },
  },
}
```

### Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `mcp.enabled` | bool | `false` | Global kill switch |
| `mcp.servers` | table | `{}` | Array of server configs |
| `servers[].name` | string | required | Unique server name (used in tool prefix) |
| `servers[].enabled` | bool | `true` | Skip if false |
| `servers[].transport` | string | `"stdio"` | `stdio`, `http`, or `streamable_http` |
| `servers[].command` | string | required for stdio | Executable to spawn |
| `servers[].args` | table | `{}` | Arguments passed to command |
| `servers[].env` | table | `{}` | Extra environment variables |
| `servers[].url` | string | required for HTTP | Streamable HTTP MCP endpoint |
| `servers[].headers` | table | `{}` | Extra HTTP headers, usually auth |
| `servers[].timeout` | number | `30000` | Per-tool-call timeout in ms |

## Architecture

### C layers: `src/mcp.c` and `src/http.c`

Provides `mcp` Lua global with bidirectional subprocess management:

```lua
mcp.spawn(command, args, env)  → handle (integer) | nil, error
mcp.send(handle, line)         → true | nil, error
mcp.recv(handle, timeout_ms)   → line (string) | nil, error
mcp.alive(handle)              → boolean
mcp.kill(handle)               → void
```

Implementation: `fork()` + `pipe()` for stdin/stdout, `execvp()`/`execve()` in
the child, plus a close-on-exec error pipe so failed `exec` calls are reported
to the parent during spawn. `recv()` uses `select()` with timeout, reads
line-by-line, and closes/reaps dead subprocesses before returning errors. UI is
pumped via `tui_pump_blocking()` during blocking reads and graceful shutdown.

`src/http.c` also exposes:

```lua
http.post_response(url, body, headers, timeout_ms)
  → {status=number, body=string, headers=table}
```

The headers table uses lowercase header names. MCP HTTP uses this to capture
`mcp-session-id` and detect `content-type`.

### Lua layer: `agent/mcp.lua`

```lua
local mcp_client = require("agent.mcp")

mcp_client.init(config)              — spawn + initialize all servers
mcp_client.collect_tools()           — return OpenAI-format tool definitions
mcp_client.call(tool_name, args)     — route and execute MCP tool call
mcp_client.list_servers()            — return server status info
mcp_client.restart(server_name)      — kill + respawn a server
mcp_client.shutdown()                — kill all servers
```

### Integration points in `agent/tools.lua`

1. **`M.collect()`** — after plugin tools, append `mcp_client.collect_tools()`.
2. **`call_plugin_tool()`** — if tool name matches a known exposed MCP tool,
   route to `mcp_client.call()` instead of plugin handler.
3. **`tool_permission_name()`** — MCP tools return `"mcp"` as permission key.

### Plugin: `plugins/mcp.lua`

Slash command `/mcp`:
- `/mcp` — list servers and their status
- `/mcp tools <server>` — list tools from a server
- `/mcp restart <server>` — restart a server
- `/mcp off` — disconnect all servers

## Constraints

- **Single-threaded**: tool calls block the main loop. The UI is pumped via
	  `tui_pump_blocking()` during blocking reads, so the spinner stays animated.
- **HTTP is request/response first**: finite JSON and finite SSE responses are
  supported. A separate long-lived server-to-client stream can be added when a
  connector requires it.
- **No sampling**: MCP servers cannot request LLM completions from capstan
  (the `sampling` capability is not advertised).
- **No resources/prompts**: only `tools` are supported. Resources and prompts
  are future enhancements.
- **No notifications**: `notifications/tools/list_changed` from servers are
  ignored in the initial version (tool list is cached at startup).

## Test notes

- `src/mcp.c` subprocess logic depends on fork/pipe/select — not unit-testable
  without a real subprocess. Tested via integration (manual).
- `agent/mcp.lua` JSON-RPC framing can be tested in pure Lua if extracted into
  a separate function. Future: `test/test_mcp.c` for framing logic.
- The exposed-name routing logic (`mcp__server__tool` → original MCP
  server/tool mapping) is pure logic and can be unit tested.
