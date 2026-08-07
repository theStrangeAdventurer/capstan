# Agent Client Protocol (ACP)

## Purpose

Capstan exposes an ACP v1 agent over newline-delimited JSON-RPC 2.0 on standard
input and standard output:

```sh
capstan acp [--yolo]
```

`--yolo` automatically permits interactive permission decisions for every ACP
session in the process while preserving explicit deny rules.

The mode is intended for IDEs and external orchestrators. Standard output is
reserved for protocol messages; diagnostics and runtime errors go to standard
error.

## Architecture

ACP is an adapter over the existing `capstan.agent.run` runtime, not a second
agent implementation:

- `src/acp.c` owns stdio transport, polling, cancellation, workspace switching,
  and the bridge to C registries;
- `agent/acp.lua` owns JSON-RPC dispatch, ACP sessions, content conversion,
  configuration, and event projection;
- `agent/runtime.lua` remains the owner of model requests and continuation;
- `agent/tools.lua` publishes transport-neutral tool lifecycle callbacks and
  delegates permission decisions to a caller callback when one is provided.

The process can hold several logical sessions, but only one prompt may execute
at a time because the current Lua, HTTP/provider, workspace, and permission
execution state is process scoped. MCP connections and tool visibility are
session scoped, and different sessions retain independent message histories and
settings.

## Supported ACP v1 surface

- `initialize`
- `session/new`
- `session/prompt`
- `session/cancel`
- `session/set_config_option`
- `session/close`
- `session/update` notifications for text, tools, and commands
- `session/request_permission` with allow-once, allow-always, and reject
- text, image, and embedded text-resource prompt blocks
- configured model selection, `mode` (`fast`, `implement`, `plan`), and
  reasoning `effort` options
- legacy `session/set_mode` compatibility and `current_mode_update`
- built-in and user plugin commands through `available_commands_update`

The initialization response advertises `sessionCapabilities.close` and omits
unsupported optional capabilities. `initialize` is accepted exactly once per
process; subsequent calls fail with JSON-RPC `-32600`. Valid notifications never
receive success or error responses. Only `session/cancel` currently has
notification semantics; other request-only methods sent without an `id` are
ignored.

Session load/list/resume/fork, client filesystem/terminal operations,
usage/thought streaming, and rich terminal/diff tool output are not advertised
yet.

Model setup is intentionally network-free. The selector contains the models
explicitly configured for Capstan providers; it does not fetch every provider's
remote model catalog while `session/new` is blocking the ACP request loop.

ACP image blocks require an `image/*` `mimeType` and base64 `data`. The adapter
uses the shared multimodal validator, enforces the 10 MiB decoded-size limit,
and converts accepted input to the canonical OpenAI-compatible `image_url`
data block. This supports clipboard images when the ACP client encodes the
clipboard blob as an image content block; clipboard access itself belongs to
the client application, not the stdio agent.

## Sessions and workspace

`session/new.cwd` must be an existing absolute directory. Before each prompt,
Capstan makes that directory both the working directory and workspace permission
boundary. Prompts and new-session requests are rejected while another run is
active so process-global paths cannot change underneath a running tool call.

Sessions are currently process-local and are discarded when the ACP process
exits. Durable session operations must reuse the existing Capstan session store
rather than create a parallel ACP persistence format.

## MCP servers

Capstan's normal MCP client remains available in ACP mode. Servers configured in
`capstan.config.mcp.servers` are initialized by the shared runtime and their
tools are exposed to ACP prompts under the usual `mcp__<server>__<tool>` names.
Before dispatching the first model-backed ACP prompt, the adapter waits for
configured MCP discovery to finish (or fail), so successfully discovered tools
are included in that prompt rather than appearing only on a later turn.

`session/new.mcpServers` adds MCP servers owned by that ACP session. Capstan
supports the mandatory stdio transport and advertises Streamable HTTP support;
legacy SSE is not advertised. Descriptors are validated against the ACP shape,
including absolute stdio commands, dense argument/environment/header arrays,
and `http`/`https` URLs.

The MCP runtime stores each session's servers in a separate registry. Tool
collection and invocation receive the exact session scope and expose only the
process-wide configured tools plus tools owned by that session. Server names
that would collide with a configured server after provider-safe normalization
are rejected. Different sessions may use the same server name without sharing
connections or tools.

Before a prompt is dispatched, ACP waits for both configured and session-owned
MCP discovery to finish or fail. `session/close` and ACP disconnect terminate
the session's stdio processes. For stateful Streamable HTTP servers, close also
sends `DELETE` with the negotiated `Mcp-Session-Id`; stateless HTTP servers need
no termination request. Environment variables, headers, tool discovery, calls,
and permission decisions remain within the owning ACP session.

## Streaming and tools

Text chunks become `agent_message_chunk` updates. Tool calls become a
`tool_call` followed by a `tool_call_update` with `completed` or `failed`
status. Callback failures are logged and do not abort the agent run.

## Permissions

ACP tool execution preserves Capstan's existing permission policy. An `allow`
or `deny` decision is honored directly. A decision requiring interaction sends
`session/request_permission` to the client and pauses the run guard while the
client chooses allow once, always allow, or reject. Always-allow decisions are
kept in the ACP session's in-memory permission scope and are not written to
Capstan's permission store. Disconnects, cancellation, malformed responses,
and client errors fail closed.

## Cancellation

`session/cancel` cancels active HTTP streams and resolves the outstanding prompt
with `stopReason: "cancelled"`. Disconnecting stdin applies the same transport
cancellation without attempting to write a final response.

## Subsequent steps

### 1. Track active work by session and turn identifiers

Replace the single global `active` slot and implicit permission wait with
registries keyed by session and turn/request identifiers. Keep provider and MCP
constraints explicit; concurrency may initially remain serialized while state
ownership becomes precise.

**Почему это надо сделать:** cancellation, permission responses, and late stream
callbacks must be routed to the exact turn. Identifier-based ownership avoids
cross-session completion and makes later safe concurrency possible without
changing the protocol surface again.

### 2. Derive ACP modes and configuration from canonical runtime registries

Expose profile metadata and model/config option metadata from their current
runtime owners, then build ACP `modes` and `configOptions` from those APIs rather
than duplicating `fast`, `implement`, and `plan` in the adapter.

**Почему это надо сделать:** duplicated lists drift when profiles or model
settings change. Dynamic metadata keeps ACP clients consistent with the CLI and
user configuration.

### 3. Project richer runtime events into ACP updates

Add plan updates, session title/time updates, usage and thought events when
supported, and richer terminal/file-diff tool content. Preserve stable tool-call
identifiers and explicit pending/completed/failed/cancelled statuses.

**Почему это надо сделать:** IDE clients need structured state to render useful
progress and diffs; flattening everything to text loses information already
available inside the Capstan runtime.

### 4. Add durable session operations

Implement `session/list`, `session/load`, `session/resume`, and `session/fork` on
top of Capstan's existing session store, including replay of history and session
metadata. Do not introduce an ACP-only persistence format.

**Почему это надо сделать:** process-local sessions cannot survive editor or
agent restarts. Reusing the canonical store preserves one history model across
TUI, CLI, and ACP and avoids incompatible copies of the same conversation.

### 5. Negotiate optional client capabilities before server requests

Store client capabilities from `initialize` and gate future elicitation,
filesystem, terminal, and ACP-bridged MCP requests on the exact advertised
capability. Unsupported operations must fail closed with actionable errors.

**Почему это надо сделать:** optional ACP methods are not universally available.
Capability checks prevent hangs and accidental fallback behavior when a client
cannot answer a server-initiated request.

## Test notes

- CLI parsing is covered by `test/test_cli_args.c`.
- `test/test_acp.sh` launches the built binary with isolated state and verifies
  initialization, session creation, config options, command publication, image
  validation, canonical image conversion, configured MCP discovery, and
  first-prompt tool exposure. It also verifies session-owned stdio and HTTP MCP
  discovery, cross-session tool isolation, real tool-call routing, permission
  approval, stdio process termination, HTTP session deletion, and rich tool
  updates.
- Runtime callback changes are exercised by the existing Lua/tool test suite.
