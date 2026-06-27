local mcp_client = require("agent.mcp")

local function format_servers()
  local servers = mcp_client.list_servers()
  if #servers == 0 then
    return "No MCP servers configured.\n\nAdd an `mcp` section to your config.lua to get started."
  end

  local lines = { "MCP Servers:", "" }
  for _, s in ipairs(servers) do
    local status_icon = "●"
    if s.status == "connected" then
      status_icon = "✓"
    elseif s.status == "failed" then
      status_icon = "✗"
    elseif s.status == "connecting" then
      status_icon = "…"
    else
      status_icon = "○"
    end

    local line = string.format("  %s %s — %s", status_icon, s.name, s.status)
    if s.transport then
      line = line .. string.format(" [%s]", s.transport)
    end
    if s.tools_count > 0 then
      line = line .. string.format(" (%d tools)", s.tools_count)
    end
    if s.error then
      line = line .. "\n      error: " .. s.error
    end
    table.insert(lines, line)
  end
  return table.concat(lines, "\n")
end

local function format_tools(server_name)
  local servers = mcp_client.list_servers()
  for _, s in ipairs(servers) do
    if s.name == server_name then
      if #s.tools == 0 then
        return "No tools from server '" .. server_name .. "'"
      end
      local lines = { "Tools from " .. server_name .. ":", "" }
      for _, t in ipairs(s.tools) do
        table.insert(lines, "  • " .. t)
      end
      return table.concat(lines, "\n")
    end
  end
  return "Unknown server: " .. tostring(server_name)
end

return {
  id = "mcp",
  name = "MCP",
  description = "MCP server management",
  command = "/mcp",
  handler = function(ctx)
    local args = ctx.args or {}

    if #args == 0 then
      return ctx:replace(format_servers())
    end

    local subcommand = args[1]

    if subcommand == "tools" then
      if not args[2] then
        return ctx:replace("Usage: /mcp tools <server>")
      end
      return ctx:replace(format_tools(args[2]))
    end

    if subcommand == "restart" then
      if not args[2] then
        return ctx:replace("Usage: /mcp restart <server>")
      end
      local ok, err = mcp_client.restart(args[2])
      if ok then
        return ctx:replace("Restarted '" .. args[2] .. "' — connected")
      else
        return ctx:replace("Failed to restart '" .. args[2] .. "': " .. tostring(err or "unknown error"))
      end
    end

    if subcommand == "off" then
      mcp_client.shutdown()
      return ctx:replace("All MCP servers disconnected.")
    end

    return ctx:replace("Usage: /mcp [tools <server> | restart <server> | off]")
  end,
}
