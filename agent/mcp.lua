local json = require("vendor.rxi.json")
local logging = require("agent.logging")
local image_runtime = require("agent.images")

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
local initialized = false
local initializing = false
local disabled = false
local init_started = false
local init_configs = {}
local init_index = 1

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

local function now_ms()
  if _G.capstan and type(_G.capstan.now_ms) == "function" then
    return _G.capstan.now_ms()
  end
  return math.floor(os.clock() * 1000)
end

local function async_rpc_start(server, method, params, timeout_ms)
  if not server then
    return false, "server not connected"
  end
  local id = next_id
  next_id = next_id + 1
  local payload = json.encode(build_rpc_request(id, method, params))
  server.pending = {
    id = id,
    method = method,
    started_at = now_ms(),
    timeout = timeout_ms or server.timeout or 30000,
  }

  if server.transport == "http" then
    if not http or type(http.post_response_async) ~= "function" then
      server.pending = nil
      return false, "http.post_response_async is not available"
    end
    log("send " .. method .. " id=" .. tostring(id) .. " to " .. server.name .. " over HTTP async")
    local ok, async_id_or_err = pcall(http.post_response_async, server.url, payload,
                                      http_headers(server), timeout_ms or server.timeout or 30000,
                                      function(response, err, err_body)
                                        server.pending_response = response
                                        server.pending_error = err
                                        server.pending_error_body = err_body
                                      end,
                                      {background = true})
    if not ok then
      server.pending = nil
      return false, "HTTP async request failed: " .. tostring(async_id_or_err)
    end
    server.pending.async_id = async_id_or_err
    return true
  end

  if not server.handle then
    server.pending = nil
    return false, "server not connected"
  end

  log("send " .. method .. " id=" .. tostring(id) .. " to " .. server.name .. " async")
  local ok, err = mcp.send(server.handle, payload)
  if not ok then
    server.pending = nil
    return false, "send failed: " .. tostring(err)
  end
  return true
end

local function async_rpc_poll(server)
  local pending = server and server.pending
  if not pending then
    return nil
  end

  if now_ms() - pending.started_at >= pending.timeout then
    local method = pending.method
    server.pending = nil
    return false, "timeout waiting for response to " .. tostring(method)
  end

  if server.transport == "http" then
    if server.pending_response == nil and server.pending_error == nil then
      return nil
    end
    local response = server.pending_response
    local err = server.pending_error
    local err_body = server.pending_error_body
    server.pending = nil
    server.pending_response = nil
    server.pending_error = nil
    server.pending_error_body = nil
    if err then
      return false, tostring(err) .. (err_body and (": " .. logging.compact(err_body, 500)) or "")
    end
    return decode_http_rpc_response(server, pending.method, pending.id, response)
  end

  for _ = 1, 8 do
    local ok_recv, line, rerr = pcall(mcp.recv_nowait, server.handle)
    if not ok_recv then
      server.pending = nil
      return false, "recv crashed: " .. tostring(line)
    end
    if not line then
      if rerr == "again" then
        return nil
      end
      server.pending = nil
      return false, "recv failed: " .. tostring(rerr)
    end

    if line ~= "" then
      local ok_parse, msg = pcall(json.decode, line)
      if not ok_parse then
        log("ignoring unparseable line from " .. server.name .. ": " .. logging.compact(line, 200))
      elseif msg.id == nil then
        log("notification from " .. server.name .. ": " .. tostring(msg.method))
      elseif msg.id ~= pending.id then
        log("ignoring response with mismatched id=" .. tostring(msg.id) .. " (expected " .. tostring(pending.id) .. ")")
      else
        server.pending = nil
        if msg.error then
          return false, "JSON-RPC error: " .. tostring(msg.error.message or "unknown")
        end
        return msg.result or {}
      end
    end
  end

  return nil
end

--- Send a notification (no response expected)
local function rpc_notify(server, method, params)
  if not server or (server.status ~= "connected" and server.status ~= "connecting") then
    return false, "server not connected"
  end

  local payload = json.encode(build_rpc_notification(method, params))
  if server.transport == "http" then
    if not http or type(http.post_response_async) ~= "function" then
      return false, "http.post_response_async is not available"
    end
    local ok, err = pcall(http.post_response_async, server.url, payload, http_headers(server), server.timeout or 30000,
                          function(response)
                            apply_http_session(server, response)
                          end,
                          {background = true})
    if not ok then
      return false, tostring(err)
    end
    return true
  end

  return mcp.send(server.handle, payload)
end

local function configured_server_count()
  local n = 0
  for _, cfg in ipairs(init_configs) do
    if type(cfg) == "table" and cfg.name and (cfg.command or cfg.url) then
      n = n + 1
    end
  end
  return n
end

local function start_server_async(cfg)
  if cfg.enabled == false then
    log("skipping disabled server: " .. tostring(cfg.name))
    return nil
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
    phase = "spawn",
    pending = nil,
  }
  servers[cfg.name] = server

  if server.transport == "stdio" then
    local args = cfg.args or {}
    local env = cfg.env or {}
    log("spawning server " .. cfg.name .. ": " .. cfg.command .. " " .. json.encode(args))

    local handle, spawn_err = mcp.spawn(cfg.command, args, env)
    if not handle then
      server.status = "failed"
      server.error = "spawn failed: " .. tostring(spawn_err)
      server.phase = "done"
      log(server.error)
      return server
    end
    server.handle = handle
  elseif server.transport == "http" then
    if type(server.url) ~= "string" or server.url == "" then
      server.status = "failed"
      server.error = "HTTP MCP server missing url"
      server.phase = "done"
      log(server.error)
      return server
    end
    log("connecting HTTP server " .. cfg.name .. ": " .. server.url)
  else
    server.status = "failed"
    server.error = "unsupported MCP transport: " .. tostring(server.transport)
    server.phase = "done"
    log(server.error)
    return server
  end

  local init_params = {
    protocolVersion = protocol_version,
    capabilities = {},
    clientInfo = {
      name = "capstan",
      version = "1.0.0",
    },
  }
  local ok, err = async_rpc_start(server, "initialize", init_params, 15000)
  if not ok then
    mark_server_failed(server, "initialize send failed: " .. tostring(err))
    server.phase = "done"
    return server
  end
  server.phase = "initialize"
  return server
end

local function fail_async_server(server, message)
  server.status = "failed"
  server.error = message
  server.phase = "done"
  log(message)
  if server.handle then
    pcall(mcp.kill, server.handle)
    server.handle = nil
  end
end

local function tick_server(server)
  if not server or server.phase == "done" then
    return false
  end
  if server.phase == "initialize" then
    local result, err = async_rpc_poll(server)
    if result == nil and err == nil then
      return false
    end
    if result == false then
      fail_async_server(server, "initialize failed: " .. tostring(err))
      return true
    end
    log("initialized " .. server.name .. " — server: " .. tostring(result.serverInfo and result.serverInfo.name or "unknown") .. " v" .. tostring(result.serverInfo and result.serverInfo.version or "?"))
    rpc_notify(server, "notifications/initialized")
    local ok, send_err = async_rpc_start(server, "tools/list", nil, 10000)
    if not ok then
      fail_async_server(server, "tools/list send failed: " .. tostring(send_err))
      return true
    end
    server.phase = "tools"
    return true
  end

  if server.phase == "tools" then
    local result, err = async_rpc_poll(server)
    if result == nil and err == nil then
      return false
    end
    if result == false then
      fail_async_server(server, "tools/list failed: " .. tostring(err))
      return true
    end
    server.tools = {}
    if type(result.tools) == "table" then
      for _, t in ipairs(result.tools) do
        table.insert(server.tools, {
          name = t.name,
          description = t.description or "",
          inputSchema = t.inputSchema or {type = "object", properties = {}},
        })
      end
    end
    server.status = "connected"
    server.phase = "done"
    log("connected " .. server.name .. " (" .. #server.tools .. " tools)")
    return true
  end

  return false
end

local function all_init_done()
  if not init_started then return false end
  if init_index <= #init_configs then return false end
  for _, server in pairs(servers) do
    if server.phase and server.phase ~= "done" then
      return false
    end
  end
  return true
end

--- Start initializing configured MCP servers without waiting for them.
function M.init()
  if disabled then
    log("MCP disabled for this runtime")
    return
  end
  if initialized or init_started then
    return
  end
  initializing = true
  if not mcp_enabled() then
    log("MCP not enabled in config")
    initialized = true
    initializing = false
    return
  end

  init_configs = servers_config()
  init_index = 1
  init_started = true
  local total = configured_server_count()
  if total == 0 then
    initialized = true
    initializing = false
    log("MCP: no servers configured")
  else
    log("MCP background init queued (" .. tostring(total) .. " servers)")
  end
end

function M.disable()
  disabled = true
  initialized = true
  init_started = true
  initializing = false
end

function M.ensure_initialized()
  if not initialized and not init_started then
    M.init()
  end
end

function M.tick(max_steps)
  if disabled or initialized then
    return false
  end
  M.ensure_initialized()
  if initialized then
    return false
  end

  local steps = max_steps or 1
  if steps < 1 then steps = 1 end
  local changed = false

  for _ = 1, steps do
    local started_one = false
    while init_index <= #init_configs do
      local cfg = init_configs[init_index]
      init_index = init_index + 1
      if type(cfg) == "table" and cfg.name and (cfg.command or cfg.url) then
        start_server_async(cfg)
        started_one = true
        changed = true
        break
      end
    end
    if not started_one then
      break
    end
  end

  for _, server in pairs(servers) do
    if tick_server(server) then
      changed = true
    end
  end

  if all_init_done() then
    initialized = true
    initializing = false
    log("MCP background init complete")
  end

  return changed
end

--- Collect all MCP tools in OpenAI function-calling format.
--- Tool names are prefixed: "mcp__server__original_name"
function M.collect_tools()
  M.tick(1)
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
--- Returns a string for text-only results or {text, images} for multimodal
--- results, followed by ok (boolean).
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

  -- Extract typed content without logging or flattening image payloads.
  local parts = {}
  local result_images = {}
  if type(result.content) == "table" then
    for _, item in ipairs(result.content) do
      if item.type == "text" and item.text then
        table.insert(parts, normalize_relative_paths(item.text))
      elseif item.type == "image" then
        local image, image_err = image_runtime.from_mcp(item)
        if image then
          table.insert(result_images, image)
          table.insert(parts, string.format("[image attached: %s, %d bytes]",
            image.mime_type, image.bytes))
        elseif image_err == "too_large" then
          table.insert(parts, "[image omitted: exceeds 10 MiB limit]")
        else
          table.insert(parts, "[image omitted: invalid MCP image content]")
        end
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

  log(string.format("tool %s ok: text_bytes=%d images=%d",
    tool_name, #text, #result_images))
  if #result_images > 0 then
    return {text = text, images = result_images}, true
  end
  return text, true
end

--- List all servers and their status
function M.list_servers()
  local list = {}
  if not init_started and not initialized and mcp_enabled() then
    for _, cfg in ipairs(servers_config()) do
      if type(cfg) == "table" and cfg.name and (cfg.command or cfg.url) then
        table.insert(list, {
          name = cfg.name,
          status = "pending",
          error = nil,
          transport = server_transport(cfg),
          tools_count = 0,
          tools = {},
        })
      end
    end
    table.sort(list, function(a, b) return a.name < b.name end)
    return list
  end
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
  if init_started and not initialized then
    for i = init_index, #init_configs do
      local cfg = init_configs[i]
      if type(cfg) == "table" and cfg.name and (cfg.command or cfg.url) and
         not servers[cfg.name] then
        table.insert(list, {
          name = cfg.name,
          status = "pending",
          error = nil,
          transport = server_transport(cfg),
          tools_count = 0,
          tools = {},
        })
      end
    end
  end
  table.sort(list, function(a, b) return a.name < b.name end)
  return list
end

--- Restart a specific server
function M.restart(server_name)
  M.ensure_initialized()
  local server = servers[server_name]
  local cfg = server and server.config
  if not cfg then
    for _, candidate in ipairs(servers_config()) do
      if type(candidate) == "table" and candidate.name == server_name then
        cfg = candidate
        break
      end
    end
  end
  if not cfg then
    return false, "unknown server: " .. tostring(server_name)
  end

  if server and server.handle then
    mcp.kill(server.handle)
  end
  servers[server_name] = nil
  start_server_async(cfg)
  initialized = false
  initializing = true
  return true
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
