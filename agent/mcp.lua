local json = require("vendor.rxi.json")
local logging = require("agent.logging")

--[[
  MCP client — connects to MCP servers over stdio, discovers tools,
  routes tool calls.

  Tool naming: server tools are exposed as "mcp__server__tool_name" to avoid
  collisions with built-in plugin tools while satisfying provider function-name
  regexes that reject dots.
]]

local M = {}

-- Server registry: name → { handle, tools, status, config }
local servers = {}
local exposed_tool_names = {}
local protocol_version = "2025-06-18"

-- JSON-RPC request ID counter
local next_id = 1

local function log(msg)
  logging.runtime_log("mcp", msg)
end

local function compact_json(value)
  local ok, encoded = pcall(json.encode, value)
  if not ok then
    return tostring(value)
  end
  return logging.compact(encoded, 500)
end

local function is_absolute_path(path)
  return type(path) == "string" and path:sub(1, 1) == "/"
end

local function normalize_workdir_path(path)
  if type(path) ~= "string" or path == "" or is_absolute_path(path) then
    return path
  end
  if path:sub(1, 2) == "./" then
    path = path:sub(3)
  end
  local workdir = _G.capstan and _G.capstan.workdir
  if type(workdir) ~= "string" or workdir == "" then
    return path
  end
  return workdir:gsub("/+$", "") .. "/" .. path
end

local function normalize_relative_paths(text)
  if type(text) ~= "string" or text == "" then
    return text
  end

  local normalized = text:gsub("%./([%w%._%-]+%.png)", function(name)
    return normalize_workdir_path(name)
  end)
  normalized = normalized:gsub("%./([%w%._%-]+%.jpg)", function(name)
    return normalize_workdir_path(name)
  end)
  normalized = normalized:gsub("%./([%w%._%-]+%.jpeg)", function(name)
    return normalize_workdir_path(name)
  end)
  normalized = normalized:gsub("%./([%w%._%-]+%.webp)", function(name)
    return normalize_workdir_path(name)
  end)
  return normalized
end

local function mark_server_failed(server, reason)
  if not server then return end
  server.status = "failed"
  server.error = reason
  if server.handle then
    pcall(mcp.kill, server.handle)
    server.handle = nil
  end
end

local function clone_default(value)
  if type(value) ~= "table" then
    return value
  end
  local copy = {}
  for k, v in pairs(value) do
    copy[k] = clone_default(v)
  end
  return copy
end

local function apply_schema_defaults(schema, args)
  if type(schema) ~= "table" or type(schema.properties) ~= "table" then
    return args or {}
  end

  local normalized = args or {}
  for name, property in pairs(schema.properties) do
    if normalized[name] == nil and type(property) == "table" and property.default ~= nil then
      normalized[name] = clone_default(property.default)
    end
  end
  return normalized
end

local function safe_tool_name_part(value)
  local part = tostring(value or "")
  part = part:gsub("[^%w_-]", "_")
  if part == "" then
    return "tool"
  end
  return part
end

local function exposed_tool_name(server_name, tool_name)
  return "mcp__" .. safe_tool_name_part(server_name) .. "__" .. safe_tool_name_part(tool_name)
end

local function remember_exposed_tool(name, server_name, tool_name)
  exposed_tool_names[name] = {
    server = server_name,
    tool = tool_name,
  }
end

local function resolve_exposed_tool(tool_name)
  if type(tool_name) ~= "string" then return nil end
  local mapped = exposed_tool_names[tool_name]
  if mapped then return mapped.server, mapped.tool end
  local server_name, mcp_tool = tool_name:match("^mcp__(.-)__(.+)$")
  if server_name and servers[server_name] then
    return server_name, mcp_tool
  end
  return nil
end

local function server_transport(cfg)
  local transport = cfg and cfg.transport
  if transport == nil or transport == "" then
    if cfg and cfg.url then return "http" end
    return "stdio"
  end
  if transport == "streamable_http" then return "http" end
  return transport
end

local function build_rpc_request(id, method, params)
  local request = {
    jsonrpc = "2.0",
    id = id,
    method = method,
  }
  if params then
    request.params = params
  end
  return request
end

local function build_rpc_notification(method, params)
  local notification = {
    jsonrpc = "2.0",
    method = method,
  }
  if params then
    notification.params = params
  end
  return notification
end

--- Check if MCP is globally enabled in config
local function mcp_enabled()
  if not _G.capstan or type(_G.capstan.config) ~= "table" then
    return false
  end
  local mcp_cfg = _G.capstan.config.mcp
  if type(mcp_cfg) ~= "table" then return false end
  return mcp_cfg.enabled == true
end

--- Get the servers config table
local function servers_config()
  if not _G.capstan or type(_G.capstan.config) ~= "table" then
    return {}
  end
  local mcp_cfg = _G.capstan.config.mcp
  if type(mcp_cfg) ~= "table" then return {} end
  local servers_list = mcp_cfg.servers
  if type(servers_list) ~= "table" then return {} end
  return servers_list
end

local function parse_sse_messages(body)
  local messages = {}
  if type(body) ~= "string" or body == "" then
    return messages
  end

  body = body:gsub("\r\n", "\n")
  for block in (body .. "\n"):gmatch("(.-)\n\n") do
    local data = {}
    for line in block:gmatch("([^\n]*)\n?") do
      if line:sub(1, 5) == "data:" then
        local value = line:sub(6)
        if value:sub(1, 1) == " " then
          value = value:sub(2)
        end
        table.insert(data, value)
      end
    end
    if #data > 0 then
      local ok, msg = pcall(json.decode, table.concat(data, "\n"))
      if ok then
        table.insert(messages, msg)
      else
        log("ignoring unparseable SSE data: " .. logging.compact(table.concat(data, "\n"), 200))
      end
    end
  end
  return messages
end

local function response_message_for_id(messages, id)
  for _, msg in ipairs(messages) do
    if msg.id == nil then
      log("notification from HTTP MCP server: " .. tostring(msg.method))
    elseif msg.id == id then
      return msg
    else
      log("ignoring response with mismatched id=" .. tostring(msg.id) .. " (expected " .. tostring(id) .. ")")
    end
  end
  return nil
end

local function apply_http_session(server, response)
  local headers = response and response.headers
  if type(headers) ~= "table" then return end
  local session_id = headers["mcp-session-id"]
  if type(session_id) == "string" and session_id ~= "" then
    server.session_id = session_id
  end
end

local function http_headers(server)
  local headers = {}
  if type(server.config.headers) == "table" then
    for k, v in pairs(server.config.headers) do
      headers[k] = v
    end
  end
  headers["Content-Type"] = headers["Content-Type"] or "application/json"
  headers["Accept"] = headers["Accept"] or "application/json, text/event-stream"
  headers["MCP-Protocol-Version"] = headers["MCP-Protocol-Version"] or protocol_version
  if server.session_id and server.session_id ~= "" then
    headers["Mcp-Session-Id"] = server.session_id
  end
  return headers
end

local function decode_http_rpc_response(server, method, id, response)
  if type(response) ~= "table" then
    return nil, "invalid HTTP response"
  end
  apply_http_session(server, response)

  local status = tonumber(response.status or 0) or 0
  local body = response.body or ""
  if status < 200 or status >= 300 then
    return nil, "HTTP " .. tostring(status) .. ": " .. logging.compact(body, 500)
  end
  if body == "" then
    return nil, "empty HTTP response to " .. method
  end

  local headers = response.headers or {}
  local content_type = tostring(headers["content-type"] or "")
  local msg = nil
  if content_type:find("text/event%-stream") then
    msg = response_message_for_id(parse_sse_messages(body), id)
    if not msg then
      return nil, "SSE response did not contain id " .. tostring(id)
    end
  else
    local ok_parse, parsed = pcall(json.decode, body)
    if not ok_parse then
      return nil, "invalid JSON response: " .. tostring(parsed)
    end
    if type(parsed) == "table" and parsed[1] ~= nil then
      msg = response_message_for_id(parsed, id)
      if not msg then
        return nil, "batched response did not contain id " .. tostring(id)
      end
    else
      msg = parsed
    end
  end

  if msg.error then
    return nil, "JSON-RPC error: " .. tostring(msg.error.message or "unknown")
  end
  return msg.result, nil
end

local function http_rpc_call(server, method, params, timeout_ms)
  if not server or server.status ~= "connected" and server.status ~= "connecting" then
    return nil, "server not connected"
  end
  if not http or type(http.post_response) ~= "function" then
    return nil, "http.post_response is not available"
  end

  local id = next_id
  next_id = next_id + 1
  local payload = json.encode(build_rpc_request(id, method, params))
  log("send " .. method .. " id=" .. tostring(id) .. " to " .. server.name .. " over HTTP")

  local ok, response = pcall(http.post_response, server.url, payload, http_headers(server), timeout_ms or server.timeout or 30000)
  if not ok then
    return nil, "HTTP request failed: " .. tostring(response)
  end
  return decode_http_rpc_response(server, method, id, response)
end

local function stdio_rpc_call(server, method, params, timeout_ms)
  if not server or not server.handle
     or (server.status ~= "connected" and server.status ~= "connecting") then
    return nil, "server not connected"
  end

  local id = next_id
  next_id = next_id + 1

  local payload = json.encode(build_rpc_request(id, method, params))
  log("send " .. method .. " id=" .. tostring(id) .. " to " .. server.name)

  local ok, err = mcp.send(server.handle, payload)
  if not ok then
    return nil, "send failed: " .. tostring(err)
  end

  -- Read lines until we get a response with matching id
  -- (skip notifications — server may send them between requests)
  local deadline = timeout_ms or server.timeout or 30000
  local start = _G.capstan.now_ms()

  while true do
    local elapsed = _G.capstan.now_ms() - start
    local remaining = deadline - elapsed
    if remaining <= 0 then
      return nil, "timeout waiting for response to " .. method
    end

    local recv_timeout = math.floor(remaining)
    if recv_timeout < 1 then recv_timeout = 1 end
    local ok_recv, line, rerr = pcall(mcp.recv, server.handle, recv_timeout)
    if not ok_recv then
      return nil, "recv crashed: " .. tostring(line)
    end
    if not line then
      return nil, "recv failed: " .. tostring(rerr)
    end

    -- Skip empty lines
    if line ~= "" then
      local ok_parse, msg = pcall(json.decode, line)
      if not ok_parse then
        log("ignoring unparseable line from " .. server.name .. ": " .. logging.compact(line, 200))
      else
        -- Skip notifications (no id field)
        if msg.id == nil then
          log("notification from " .. server.name .. ": " .. tostring(msg.method))
        elseif msg.id ~= id then
          log("ignoring response with mismatched id=" .. tostring(msg.id) .. " (expected " .. tostring(id) .. ")")
        else
          -- This is our response
          if msg.error then
            return nil, "JSON-RPC error: " .. tostring(msg.error.message or "unknown")
          end
          return msg.result, nil
        end
      end
    end
  end
end

--- Send a JSON-RPC request and wait for the response (blocking).
--- Returns: result table | nil, error_string
local function rpc_call(server, method, params, timeout_ms)
  if server and server.transport == "http" then
    return http_rpc_call(server, method, params, timeout_ms)
  end
  return stdio_rpc_call(server, method, params, timeout_ms)
end

--- Send a notification (no response expected)
local function rpc_notify(server, method, params)
  if not server or (server.status ~= "connected" and server.status ~= "connecting") then
    return false, "server not connected"
  end

  local payload = json.encode(build_rpc_notification(method, params))
  if server.transport == "http" then
    if not http or type(http.post_response) ~= "function" then
      return false, "http.post_response is not available"
    end
    local ok, response = pcall(http.post_response, server.url, payload, http_headers(server), server.timeout or 30000)
    if not ok then
      return false, tostring(response)
    end
    apply_http_session(server, response)
    local status = tonumber(response and response.status or 0) or 0
    if status < 200 or status >= 300 then
      return false, "HTTP " .. tostring(status)
    end
    return true
  end

  return mcp.send(server.handle, payload)
end

--- Initialize a single MCP server: spawn, handshake, discover tools
local function init_server(cfg)
  if cfg.enabled == false then
    log("skipping disabled server: " .. tostring(cfg.name))
    return
  end

  local server = {
    name = cfg.name,
    config = cfg,
    handle = nil,
    tools = {},
    status = "connecting",
    timeout = cfg.timeout or 30000,
    transport = server_transport(cfg),
    url = cfg.url,
    session_id = nil,
  }

  if server.transport == "stdio" then
    -- Spawn the subprocess
    local args = cfg.args or {}
    local env = cfg.env or {}
    log("spawning server " .. cfg.name .. ": " .. cfg.command .. " " .. json.encode(args))

    local handle, spawn_err = mcp.spawn(cfg.command, args, env)
    if not handle then
      server.status = "failed"
      server.error = "spawn failed: " .. tostring(spawn_err)
      log(server.error)
      servers[cfg.name] = server
      return
    end

    server.handle = handle
  elseif server.transport == "http" then
    if type(server.url) ~= "string" or server.url == "" then
      server.status = "failed"
      server.error = "HTTP MCP server missing url"
      log(server.error)
      servers[cfg.name] = server
      return
    end
    log("connecting HTTP server " .. cfg.name .. ": " .. server.url)
  else
    server.status = "failed"
    server.error = "unsupported MCP transport: " .. tostring(server.transport)
    log(server.error)
    servers[cfg.name] = server
    return
  end

  -- Initialize handshake
  local init_params = {
    protocolVersion = protocol_version,
    capabilities = {},
    clientInfo = {
      name = "capstan",
      version = "1.0.0",
    },
  }

  local result, err = rpc_call(server, "initialize", init_params, 15000)
  if not result then
    server.status = "failed"
    server.error = "initialize failed: " .. tostring(err)
    log(server.error)
    if server.handle then
      mcp.kill(server.handle)
      server.handle = nil
    end
    servers[cfg.name] = server
    return
  end

  log("initialized " .. cfg.name .. " — server: " .. tostring(result.serverInfo and result.serverInfo.name or "unknown") .. " v" .. tostring(result.serverInfo and result.serverInfo.version or "?"))

  -- Send initialized notification
  rpc_notify(server, "notifications/initialized")

  -- Discover tools
  local tools_result, terr = rpc_call(server, "tools/list", nil, 10000)
  if not tools_result then
    server.status = "failed"
    server.error = "tools/list failed: " .. tostring(terr)
    log(server.error)
    if server.handle then
      mcp.kill(server.handle)
      server.handle = nil
    end
    servers[cfg.name] = server
    return
  end

  if type(tools_result.tools) == "table" then
    for _, t in ipairs(tools_result.tools) do
      table.insert(server.tools, {
        name = t.name,
        description = t.description or "",
        inputSchema = t.inputSchema or {type = "object", properties = {}},
      })
    end
  end

  server.status = "connected"
  log("connected " .. cfg.name .. " (" .. #server.tools .. " tools)")
  servers[cfg.name] = server
end

--- Initialize all configured MCP servers
function M.init()
  if not mcp_enabled() then
    log("MCP not enabled in config")
    return
  end

  local configs = servers_config()
  for _, cfg in ipairs(configs) do
    if type(cfg) == "table" and cfg.name and (cfg.command or cfg.url) then
      init_server(cfg)
    end
  end
end

--- Collect all MCP tools in OpenAI function-calling format.
--- Tool names are prefixed: "mcp__server__original_name"
function M.collect_tools()
  local tools = {}
  exposed_tool_names = {}
  for server_name, server in pairs(servers) do
    if server.status == "connected" then
      for _, t in ipairs(server.tools) do
        local prefixed_name = exposed_tool_name(server_name, t.name)
        remember_exposed_tool(prefixed_name, server_name, t.name)
        table.insert(tools, {
          type = "function",
          ["function"] = {
            name = prefixed_name,
            description = t.description,
            parameters = t.inputSchema,
          },
          _mcp_server = server_name,
          _mcp_tool = t.name,
        })
      end
    end
  end
  return tools
end

--- Check if a tool name belongs to MCP.
function M.is_mcp_tool(tool_name)
  local server_name = resolve_exposed_tool(tool_name)
  return server_name ~= nil
end

--- Call an MCP tool by its exposed provider-safe name.
--- Returns: result_text (string), ok (boolean)
function M.call(tool_name, args)
  local server_name, tool_method = resolve_exposed_tool(tool_name)
  if not server_name or not tool_method then
    return "Unknown MCP tool: " .. tool_name, false
  end

  local server = servers[server_name]
  if not server then
    return "Unknown MCP server: " .. server_name, false
  end
  if server.status ~= "connected" then
    return "MCP server " .. server_name .. " is " .. server.status, false
  end

  local tool_schema = nil
  for _, t in ipairs(server.tools) do
    if t.name == tool_method then
      tool_schema = t.inputSchema
      break
    end
  end

  local params = {
    name = tool_method,
    arguments = apply_schema_defaults(tool_schema, args),
  }

  local result, err = rpc_call(server, "tools/call", params, server.timeout)

  if not result then
    log("tool " .. tool_name .. " failed: " .. tostring(err))
    if tostring(err):find("timeout", 1, true) or tostring(err):find("recv failed", 1, true)
       or tostring(err):find("recv crashed", 1, true) then
      mark_server_failed(server, tostring(err))
    end
    return "MCP tool call failed: " .. tostring(err), false
  end

  -- Extract text from content array
  local parts = {}
  if type(result.content) == "table" then
    for _, item in ipairs(result.content) do
      if item.type == "text" and item.text then
        table.insert(parts, normalize_relative_paths(item.text))
      elseif item.type == "image" then
        table.insert(parts, "[image data omitted]")
      elseif item.type == "resource" then
        if item.resource and item.resource.text then
          table.insert(parts, item.resource.text)
        end
      else
        table.insert(parts, json.encode(item))
      end
    end
  end

  local text = table.concat(parts, "\n")
  if result.isError then
    log("tool " .. tool_name .. " returned error: " .. logging.compact(text, 500))
    return text, false
  end

  log("tool " .. tool_name .. " ok: " .. compact_json(result))
  return text, true
end

--- List all servers and their status
function M.list_servers()
  local list = {}
  for name, server in pairs(servers) do
    local tool_names = {}
    for _, t in ipairs(server.tools) do
      table.insert(tool_names, t.name)
    end
    table.insert(list, {
      name = name,
      status = server.status,
      error = server.error,
      transport = server.transport,
      tools_count = #server.tools,
      tools = tool_names,
    })
  end
  table.sort(list, function(a, b) return a.name < b.name end)
  return list
end

--- Restart a specific server
function M.restart(server_name)
  local server = servers[server_name]
  if not server then
    return false, "unknown server: " .. tostring(server_name)
  end

  if server.handle then
    mcp.kill(server.handle)
    server.handle = nil
  end
  server.status = "disconnected"
  server.tools = {}

  init_server(server.config)
  return server.status == "connected"
end

--- Shutdown all servers
function M.shutdown()
  for name, server in pairs(servers) do
    if server.handle then
      log("shutting down " .. name)
      mcp.kill(server.handle)
      server.handle = nil
    end
    server.status = "disconnected"
  end
end

return M
