local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local mcp_client = require("agent.mcp")
local workspace = require("agent.workspace")
local redact = require("agent.redact")
local tool_output = require("agent.tool_output")
local ui = require("agent.ui")
local telemetry = require("agent.telemetry")

local M = {}

function M.completion_tool()
    local spec=require('plugins.completion_review').tool
    return {type='function',['function']={name=spec.name,description=spec.description,parameters=spec.parameters}}
end

local function config_table(name)
    if _G.capstan and type(_G.capstan.runtime_options) == "table" and
       _G.capstan.runtime_options.isolated then return nil end
    if not _G.capstan or type(_G.capstan.config) ~= "table" then return nil end
    local value = _G.capstan.config[name]
    return type(value) == "table" and value or nil
end

local function capability_enabled(name)
    local capabilities = config_table("capabilities")
    if name == "subagents" then
        return not capabilities or capabilities[name] ~= false
    end
    return capabilities and capabilities[name] == true
end

local function subagents_tool()
    return {
        type = "function",
        ["function"] = {
            name = "subagents",
            description = "Run multiple focused internal sub-agents in parallel and return their independent findings for orchestration. First choose the workflow and concrete tools in the orchestrator, then pass shared instructions and give each task the narrowest tools whitelist instead of making children rediscover tools.",
            parameters = {
                type = "object",
                properties = {
                    instructions = {
                        type = "string",
                        description = "Shared instructions prepended to every child task. Use this to pass skill/tool instructions already selected by the orchestrator.",
                    },
                    tasks = {
                        type = "array",
                        items = {
                            type = "object",
                            properties = {
                                id = {type = "string"},
                                task = {type = "string"},
                                instructions = {
                                    type = "string",
                                    description = "Task-specific instructions prepended before task.",
                                },
                                model = {type = "string"},
                                max_turns = {type = "integer", description = "Requested maximum child turns. Omit for the configured default. Use 2 for simple one-tool fan-out tasks, and more for exploratory work."},
                                tools = {
                                    type = "array",
                                    items = {type = "string"},
                                    description = "Narrow whitelist of tool names the child may use. Strongly recommended whenever the orchestrator already knows the required workflow/tools. Omitted means the child inherits all non-subagents parent tools.",
                                },
                            },
                            required = {"id", "task"},
                        },
                    },
                    max_concurrent = {type = "integer"},
                    background = {type = "boolean", description = "Return queued group/task IDs immediately. Use processes wait and output before drawing conclusions that depend on completion."},
                },
                required = {"tasks"},
            },
        },
    }
end

-- Gathers all available LLM tools: plugin tools + optional subagents tool.
local function plugin_tool_specs(p)
    local specs = {}
    if type(p.tools) == "table" then
        for _, tool in ipairs(p.tools) do
            if type(tool) == "table" then table.insert(specs, tool) end
        end
        return specs
    end
    if type(p.tool) == "table" then
        table.insert(specs, p.tool)
    end
    return specs
end

function M.collect(opts)
    opts = opts or {}
    local tools = {}
    if _G.plugins then
        for _, p in pairs(_G.plugins) do
            if workspace.wiki_enabled() or tostring(p.id or "") ~= "wiki" then
                for _, tool in ipairs(plugin_tool_specs(p)) do
                    if tool.name~='request_completion' and not (opts.disable_subagents and (tool.name == "tasks" or tool.name == "issues")) then
                    table.insert(tools, {
                        type = "function",
                        ["function"] = {
                            name = tool.name,
                            description = tool.description,
                            parameters = tool.parameters,
                        }
                    })
                    end
                end
            end
        end
    end
    if capability_enabled("subagents") and not opts.disable_subagents then
        table.insert(tools, subagents_tool())
    end

    -- MCP tools (collected from connected servers, empty if not configured)
    for _, t in ipairs(mcp_client.collect_tools(opts.mcp_scope)) do
        table.insert(tools, t)
    end

    return tools
end

function M.names(tools)
    local names = {}
    for _, tool in ipairs(tools or {}) do
        if tool["function"] and tool["function"].name then
            table.insert(names, tool["function"].name)
        end
    end
    table.sort(names)
    return table.concat(names, ",")
end

local function tool_available(tools, name)
    for _, tool in ipairs(tools or {}) do
        local fn = tool["function"]
        if type(fn) == "table" and fn.name == name then
            return true
        end
    end
    return false
end

local function find_plugin_tool(tool_name)
    if tool_name == "subagents" and capability_enabled("subagents") then
        return {tool = {name = "subagents"}}, {name = "subagents"}
    end
    if not _G.plugins then return nil end
    for _, p in pairs(_G.plugins) do
        for _, tool in ipairs(plugin_tool_specs(p)) do
            if tool.name == tool_name then
                return p, tool
            end
        end
    end
    return nil
end

local function now_ms()
    if _G.capstan and type(_G.capstan.now_ms) == "function" then
        return _G.capstan.now_ms()
    end
    return os.clock() * 1000
end

local function sorted_keys(tbl)
    local keys = {}
    for key in pairs(tbl or {}) do
        table.insert(keys, key)
    end
    table.sort(keys, function(a, b)
        return tostring(a) < tostring(b)
    end)
    return keys
end

local function stable_encode(value)
    local value_type = type(value)
    if value_type ~= "table" then
        local ok, encoded = pcall(json.encode, value)
        return ok and encoded or tostring(value)
    end

    local max_index = 0
    local array_like = true
    for key in pairs(value) do
        if type(key) ~= "number" or key < 1 or math.floor(key) ~= key then
            array_like = false
            break
        end
        if key > max_index then max_index = key end
    end

    if array_like then
        local parts = {}
        for i = 1, max_index do
            table.insert(parts, stable_encode(value[i]))
        end
        return "[" .. table.concat(parts, ",") .. "]"
    end

    local parts = {}
    for _, key in ipairs(sorted_keys(value)) do
        table.insert(parts, stable_encode(tostring(key)) .. ":" .. stable_encode(value[key]))
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

local function tool_signature(tool_name, args)
    return tostring(tool_name or "") .. ":" .. stable_encode(args or {})
end

local function shell_signature(args)
    args = args or {}
    return tostring(args.command or "") .. "\0" .. tostring(args.timeout or "")
end

local function guard_duration_error(guard)
    if not guard or not guard.max_duration_ms or guard.max_duration_ms <= 0 then
        return nil
    end
    local elapsed_ms = type(guard.elapsed_ms) == "function" and
        guard.elapsed_ms() or (now_ms() - guard.started_at)
    if elapsed_ms > guard.max_duration_ms then
        return string.format("agent run exceeded %ds", math.floor(guard.max_duration_ms / 1000))
    end
    return nil
end

local prompt_decision_allows

local function permission_prompt(run_ctx, permission_tool, target, details)
    local guard = run_ctx and run_ctx.guard or nil
    if guard and type(guard.pause) == "function" then guard.pause() end
    local prompt_started_at = now_ms()
    local prompt_span = telemetry.start("operation", run_ctx and run_ctx.telemetry_tool, {
        operation = "permission_wait", tool = permission_tool,
    })
    local callbacks = run_ctx and run_ctx.callbacks or nil
    local prompt = callbacks and callbacks.on_permission_request or permit.prompt
    local ok, decision = pcall(prompt, permission_tool, target, details)
    local paused_ms = math.max(0, now_ms() - prompt_started_at)
    local allowed = ok and prompt_decision_allows(decision)
    telemetry.finish(prompt_span, allowed, false, {
        ["error.category"] = not ok and "exception" or nil,
        duration_ms = paused_ms,
        purpose = not ok and "error" or (allowed and "allow" or
            (decision == "deny" and "deny" or "invalid_decision")),
    })
    if guard and type(guard.resume) == "function" then
        paused_ms = guard.resume()
    end
    logging.runtime_log("permit", string.format(
        "tool=%s target=%s prompt_wait_ms=%d",
        tostring(permission_tool), tostring(target), math.floor(paused_ms)
    ))
    local measured_ms = math.max(0, math.floor(paused_ms))
    if not ok then return nil, measured_ms, decision end
    return decision, measured_ms, nil
end

local function guard_before_tool(tool_name, args, run_ctx)
    local guard = run_ctx and run_ctx.guard
    if not guard then return nil end

    local duration_error = guard_duration_error(guard)
    if duration_error then return duration_error end

    guard.total_tool_calls = (tonumber(guard.total_tool_calls) or 0) + 1
    local max_tool_calls = tonumber(guard.max_tool_calls) or 0
    if max_tool_calls > 0 and guard.total_tool_calls > max_tool_calls then
        return string.format("too many tool calls (%d > %d)", guard.total_tool_calls, max_tool_calls)
    end

    if tool_name == "shell" then
        guard.last_tool_signature = nil
        guard.same_tool_count = 0
        local signature_shell = shell_signature(args)
        if guard.last_shell_signature == signature_shell then
            guard.same_shell_count = (tonumber(guard.same_shell_count) or 0) + 1
        else
            guard.last_shell_signature = signature_shell
            guard.same_shell_count = 1
        end
        local max_same_shell_command = tonumber(guard.max_same_shell_command) or 0
        if max_same_shell_command > 0 and guard.same_shell_count > max_same_shell_command then
            return string.format("repeated shell command (%d > %d)",
                guard.same_shell_count,
                max_same_shell_command
            )
        end
    else
        guard.last_shell_signature = nil
        guard.same_shell_count = 0
        local signature = tool_signature(tool_name, args)
        if guard.last_tool_signature == signature then
            guard.same_tool_count = (tonumber(guard.same_tool_count) or 0) + 1
        else
            guard.last_tool_signature = signature
            guard.same_tool_count = 1
        end
        local max_same_tool_call = tonumber(guard.max_same_tool_call) or 0
        if max_same_tool_call > 0 and guard.same_tool_count > max_same_tool_call then
            return string.format("repeated tool call %s (%d > %d)",
                tostring(tool_name),
                guard.same_tool_count,
                max_same_tool_call
            )
        end
    end

    return nil
end

-- Dispatches a single tool call to its plugin handler (or subagents builtin or MCP server).
local function call_plugin_tool(tool_name, args, run_ctx, permission_ctx)
    if tool_name == "tasks" and (tonumber(run_ctx and run_ctx.depth) or 0) > 0 then
        return "Parent task plans are not accessible to subagents", false
    end
    if tool_name == "issues" and (tonumber(run_ctx and run_ctx.depth) or 0) > 0 then
        return "Parent review issues are not accessible to subagents", false
    end
    if tool_name == "subagents" then
        return require("agent.subagents").run(args, run_ctx)
    end

    -- MCP tool routing: names like "mcp__browser__browser_navigate"
    local mcp_scope = run_ctx and run_ctx.mcp_scope or nil
    if mcp_client.is_mcp_tool(tool_name, mcp_scope) then
        local result, ok = mcp_client.call(tool_name, args, mcp_scope)
        return result, ok
    end

    local p, tool_spec = find_plugin_tool(tool_name)
    if not p then
        if not _G.plugins then return "No plugins loaded", false end
        return "Unknown tool: " .. tool_name, false
    end
    if type(p.handler) ~= "function" then
        return "Tool " .. tool_name .. " failed: plugin has no handler", false
    end

    local plugin_id = tostring(p.id or tool_name)
    local plugin_source = tostring(p.source_path or p._source_path or "unknown")
    local ctx = {
        input = "/" .. tool_name,
        command = p.command,
        args = {},
        tool_args = args,
        tool_name = tool_name,
        tool = tool_spec,
        permission = permission_ctx,
    }
    function ctx:replace(ui_val, llm_val)
        return ui_val, llm_val or ui_val
    end
    function ctx:error(ui_val, llm_val)
        return ui_val, llm_val or ui_val, false
    end
    local function traceback(err)
        return debug.traceback(tostring(err), 2)
    end
    local ok, ui_result, llm_result, result_ok = xpcall(function()
        return p.handler(ctx)
    end, traceback)
    if not ok then
        local args_json = "{}"
        local encoded_ok, encoded = pcall(json.encode, args or {})
        if encoded_ok then args_json = encoded end
        return table.concat({
            "Tool " .. tool_name .. " failed",
            "plugin: " .. plugin_id,
            "source: " .. plugin_source,
            "args: " .. args_json,
            "traceback:",
            tostring(ui_result),
        }, "\n"), false
    end
    return llm_result or ui_result, result_ok ~= false,
        tool_name == "file_read" and ui_result or nil
end

local function tool_permission_name(tool_name, run_ctx)
    -- MCP tools use a shared "mcp" permission key
    if mcp_client.is_mcp_tool(tool_name, run_ctx and run_ctx.mcp_scope or nil) then
        return "mcp"
    end
    local _, tool_spec = find_plugin_tool(tool_name)
    if tool_spec and tool_spec.permission == false then
        return nil
    end
    if tool_spec and tool_spec.permission and tool_spec.permission ~= "" then
        return tool_spec.permission
    end
    return tool_name
end

-- Parses the JSON-encoded arguments string from an LLM tool call.
local function should_strip_minimax_tool_markup(run_ctx)
    local provider_name = tostring(run_ctx and run_ctx.provider_name or ""):lower()
    local provider = run_ctx and run_ctx.provider or nil
    local model = tostring(provider and provider.model or ""):lower()
    return provider_name:find("minimax", 1, true) ~= nil or model:find("minimax", 1, true) ~= nil
end

local function strip_minimax_tool_markup(text)
    if type(text) ~= "string" or text == "" then return text end
    local result = text
    result = result:gsub("%]%<%]minimax%[%>%[</?[%w_%-]+>%]?", "")
    for _, tag in ipairs({"tool_call", "tool_calls", "command", "arguments", "function_call", "function"}) do
        result = result:gsub("</?" .. tag .. ">", "")
    end
    return result
end

local function sanitize_tool_value(value, run_ctx)
    if not should_strip_minimax_tool_markup(run_ctx) then return value end
    if type(value) == "string" then
        return strip_minimax_tool_markup(value)
    end
    if type(value) ~= "table" then return value end
    for k, v in pairs(value) do
        value[k] = sanitize_tool_value(v, run_ctx)
    end
    return value
end

local function sanitize_tool_arguments(raw, run_ctx)
    raw = raw or "{}"
    if not should_strip_minimax_tool_markup(run_ctx) then return raw end
    return strip_minimax_tool_markup(raw)
end

local function decode_tool_arguments(raw, run_ctx)
    local sanitized = sanitize_tool_arguments(raw, run_ctx)
    local ok, decoded = pcall(json.decode, sanitized)
    if not ok then
        return nil, "Invalid JSON arguments: " .. tostring(decoded)
    end
    if type(decoded) ~= "table" then
        return nil, "Invalid tool arguments: expected object"
    end
    return sanitize_tool_value(decoded, run_ctx), nil
end

local function tool_call_target(tool_name, args)
    if tool_name == "shell" then
        return workspace.configured_workspace_root()
    end
    local _, tool_spec = find_plugin_tool(tool_name)
    if tool_spec and type(tool_spec.permission_target) == "function" then
        local ok, target = pcall(tool_spec.permission_target, args or {})
        if ok and type(target) == "string" and target ~= "" then return target end
    end
    if tool_name == "file_read" then
        return workspace.file_read_paths(args)[1] or ""
    end
    return args.command or args.path or args.url or args.uri or tool_name
end

local function normalize_permission_target(permission_tool, target)
    if permission_tool == "file_read" and workspace.embedded_asset_name(target) then
        return target
    end
    if permission_tool == "file_read" or permission_tool == "file_write" then
        return workspace.normalize_path(target, workspace.runtime_workdir())
    end
    return target
end

-- Models occasionally select file_read after seeing an absolute Wiki path in
-- context. The Wiki is Capstan-owned state and has its own permission-free,
-- root-confined reader, so route that equivalent request to the canonical tool
-- before permission handling. External paths remain ordinary file_read calls.
local function route_internal_wiki_read(tool_name, args)
    if tool_name ~= "file_read" or type(args) ~= "table" or type(args.paths) == "table" then
        return tool_name, args, false
    end
    local relative = workspace.wiki_relative_path(args.path)
    if not relative then return tool_name, args, false end
    return "wiki_read", {path = relative}, true
end

local sensitive_headers = {
    authorization = true,
    ["proxy-authorization"] = true,
    cookie = true,
    ["set-cookie"] = true,
    ["x-api-key"] = true,
    ["api-key"] = true,
    ["openai-api-key"] = true,
    ["anthropic-api-key"] = true,
    ["x-goog-api-key"] = true,
    ["x-subscription-key"] = true,
    ["subscription-key"] = true,
}

local sensitive_keys = {
    "api_key", "api-key", "apikey",
    "access_token", "access-token",
    "refresh_token", "refresh-token",
    "id_token", "id-token",
    "auth_token", "auth-token",
    "bearer_token", "bearer-token",
    "token", "secret", "password", "passwd",
}

local function redact_sensitive_key_values(text)
    local result = tostring(text or "")
    for _, key in ipairs(sensitive_keys) do
        result = result:gsub("([\"']?%f[%w]" .. key .. "%f[^%w][\"']?%s*[:=]%s*[\"']?)[^\"'%s,;}]+", "%1[REDACTED]")
        result = result:gsub("([\"']?%f[%w]" .. key:upper() .. "%f[^%w][\"']?%s*[:=]%s*[\"']?)[^\"'%s,;}]+", "%1[REDACTED]")
    end
    return result
end

local function redact_sensitive_header_line(line)
    local curl_prefix, name = line:match("^(%s*[<>]%s*)([%w%-]+)%s*:")
    if curl_prefix and name then
        return curl_prefix .. name .. ": [REDACTED]"
    end
    name = line:match("^%s*([%w%-]+)%s*:")
    if name and sensitive_headers[name:lower()] then
        return line:gsub("(:%s*).*$", "%1[REDACTED]")
    end
    return line
end

local function redact_sensitive_text(text)
    if type(text) ~= "string" or text == "" then return text end
    return redact.text(text)
end

local function redact_sensitive_text_legacy(text)
    if type(text) ~= "string" or text == "" then return text end
    local result = text
    result = result:gsub("([Aa][Uu][Tt][Hh][Oo][Rr][Ii][Zz][Aa][Tt][Ii][Oo][Nn]%s*:%s*[Bb][Ee][Aa][Rr][Ee][Rr]%s+)[^%s\"']+", "%1[REDACTED]")
    result = result:gsub("([Aa][Uu][Tt][Hh][Oo][Rr][Ii][Zz][Aa][Tt][Ii][Oo][Nn]%s*:%s*)[^\r\n\"']+", "%1[REDACTED]")
    result = redact_sensitive_key_values(result)

    local lines = {}
    local had_line = false
    for line, newline in result:gmatch("([^\r\n]*)(\r?\n?)") do
        if line == "" and newline == "" then break end
        had_line = true
        table.insert(lines, redact_sensitive_header_line(line) .. newline)
    end
    if had_line then result = table.concat(lines) end
    return result
end

local function unquote_shell_token(token)
    if type(token) ~= "string" then return "" end
    if #token >= 2 then
        local first = token:sub(1, 1)
        local last = token:sub(-1)
        if (first == "'" and last == "'") or (first == '"' and last == '"') then
            return token:sub(2, -2)
        end
    end
    return token
end

local function summarize_shell_command(command, fallback)
    if type(command) ~= "string" or command == "" then
        return fallback or "shell"
    end

    local first = command:match("^%s*([^%s]+)")
    first = unquote_shell_token(first or "")
    if first:match("/?curl$") or first == "curl" then
        local url = nil
        for token in command:gmatch("%S+") do
            local clean_token = unquote_shell_token(token)
            if clean_token:match("^https?://") then
                url = clean_token
                break
            end
        end
        return url and ("curl " .. url) or "curl"
    end

    return fallback
end

local function redacted_tool_arguments(tool_name, raw_arguments)
    if tool_name ~= "shell" then return raw_arguments end
    return redact_sensitive_text(raw_arguments or "")
end

local function call_plugin_tool_redacted(tool_name, args, run_ctx, permission_ctx)
    local result, ok, summary = call_plugin_tool(tool_name, args, run_ctx, permission_ctx)
    if tool_name == "shell" then
        result = redact_sensitive_text(result)
    end
    return result, ok, summary
end

local function tool_result_text(result)
    if type(result) == "table" then return tostring(result.text or "") end
    return tostring(result or "")
end

local function tool_result_images(result)
    if type(result) ~= "table" or type(result.images) ~= "table" then return {} end
    return result.images
end

-- Display paths are not permission targets. Keep workspace paths short without
-- losing directory context; external and embedded references stay identifiable.
local function tool_display_path(path, base)
    if workspace.embedded_asset_name(path) then return path end
    local normalized = workspace.normalize_path(path, base)
    base = workspace.normalize_path(base)
    if workspace.path_is_within(normalized, base) then
        if normalized == base then return "." end
        return normalized:sub(#base:gsub("/+$", "") + 2)
    end
    return normalized
end

local function tool_display_target(tool_name, args, target)
    if tool_name == "shell" then return "shell" end
    if tool_name == "file_read" then
        local paths = workspace.file_read_paths(args)
        for i, path in ipairs(paths) do
            paths[i] = tool_display_path(path, workspace.configured_workdir())
        end
        return #paths > 0 and table.concat(paths, ", ") or "(no path)"
    end
    if tool_name == "vcs" then
        local operation = type(args.operation) == "string" and args.operation or "(no operation)"
        local path = type(args.path) == "string" and args.path ~= "" and
            tool_display_path(args.path, workspace.configured_workspace_root()) or "workspace"
        return operation .. " · " .. path
    end
    return target
end

local function tool_display_command(tool_name, args)
    if tool_name == "shell" and args and type(args.command) == "string" and args.command ~= "" then
        return redact_sensitive_text(args.command)
    end
    return nil
end

local shell_command_is_validation

local function tool_phase(tool_name, display_command)
    if tool_name == "file_read" or tool_name == "wiki_read" or
       tool_name == "wiki_source_read" then return "Reading" end
    if tool_name == "file_edit" or tool_name == "file_write" or
       tool_name == "wiki_ingest" then return "Editing" end
    if tool_name == "fetch" then return "Fetching" end
    if tool_name == "vcs" then return "Inspecting VCS" end
    if tool_name == "logs" then return "Inspecting logs" end
    if tool_name == "subagents" then return "Delegating" end
    if tool_name == "shell" then
        if shell_command_is_validation(display_command) then return "Validating" end
        return "Running command"
    end
    return "Using tool"
end

local function tool_activity_label(tool_name, display_command)
    local phase = tool_phase(tool_name, display_command)
    if phase ~= "Using tool" then return phase end
    return "Using " .. tostring(tool_name)
end

local function execute_tool(tool_name, args, run_ctx, permission_ctx,
                            display_command)
    local update_activity = (not run_ctx or run_ctx.update_status ~= false) and
        agent and type(agent.set_activity) == "function"
    if update_activity then
        agent.set_activity(tool_activity_label(tool_name, display_command))
    end

    local native_tools = rawget(_G, "tools")
    local scoped = native_tools and type(native_tools.process_scope) == "function"
    local previous_owner, previous_task
    if scoped then
        local span = run_ctx and run_ctx.telemetry_context
        local owner = run_ctx and (run_ctx.process_owner or run_ctx.session_id or run_ctx.mcp_scope)
        owner = owner or (span and (span.session_id or (span.context and span.context.session_id)))
        -- Missing runtime ownership fails closed: isolate this run rather than
        -- borrowing whichever interactive session happens to be active now.
        owner = owner or ("unowned:" .. tostring(run_ctx and run_ctx.state or run_ctx))
        previous_owner, previous_task = native_tools.process_scope(tostring(owner), run_ctx and run_ctx.background_id)
    end
    local values = table.pack(xpcall(function()
        return call_plugin_tool_redacted(tool_name, args, run_ctx, permission_ctx)
    end, function(err)
        return debug.traceback(tostring(err), 2)
    end))

    if scoped then native_tools.process_scope(previous_owner, previous_task) end
    if update_activity then agent.set_activity(nil) end
    if not values[1] then error(values[2], 0) end
    return table.unpack(values, 2, values.n)
end

local function tool_status_prefix(tool_name, display_target, display_command)
    local phase = tool_phase(tool_name, display_command)
    if display_command then
        return string.format("\n\n⚙ %s\n%s\n$ %s ", tool_name, phase, display_command)
    end
    return string.format("\n\n⚙ %s\n%s: %s ", tool_name, phase, display_target)
end

local function tool_status_suffix(status, _display_command)
    return status .. "\n\n"
end

local function shell_output_status(result_content, display_command, status)
    local output = tool_output.bound(redact.text(tool_result_text(result_content)))
    return tool_status_suffix(status, display_command) .. output .. "\n\n"
end

local function tool_success_status(tool_name, result_content, display_command, args)
    if tool_name == "shell" then
        return shell_output_status(result_content, display_command,
            args and args.background == true and "— started" or "— done")
    end
    if tool_name == "file_edit" and type(result_content) == "string" and result_content ~= "" then
        return "\n" .. result_content .. "\n" .. tool_status_suffix("— done", display_command)
    end
    return tool_status_suffix("— done", display_command)
end

local function first_line(value)
    local s = tostring(value or "")
    s = s:gsub("^%s+", "")
    return s:match("([^\n\r]+)") or s
end

local function tool_error_status(result_content, display_command)
    local reason = logging.compact(first_line(result_content), 160)
    if reason == "" then
        reason = "error"
    end
    if display_command then
        return shell_output_status(result_content, display_command, "— error: " .. reason)
    end
    return tool_status_suffix("— error: " .. reason, display_command)
end

local function scope_allows(scope, permission_tool)
    if not scope or not permission_tool then return false end
    if scope.full_control and not scope.workdir_only then return true end
    if scope.full_control and scope.workdir_only then
        return permission_tool == "shell" or permission_tool == "file_read" or permission_tool == "file_write"
    end
    return type(scope.allowed_tools) == "table" and scope.allowed_tools[permission_tool] == true
end

local function path_is_within_workspace(path)
    local workspace_root = workspace.configured_workspace_root()
    if type(path) ~= "string" or path == "" or type(workspace_root) ~= "string" or workspace_root == "" then
        return false
    end
    local normalized_path = workspace.normalize_path(path)
    local normalized_root = workspace.normalize_path(workspace_root)
    return workspace.path_is_within(normalized_path, normalized_root)
end

local function scope_allows_target(scope, permission_tool, target)
    if not scope or not permission_tool then return false end
    if scope.yolo then return true end
    local targets = type(scope.allowed_targets) == "table" and
        scope.allowed_targets[permission_tool] or nil
    if type(targets) == "table" and targets[tostring(target)] == true then
        return true
    end
    if not scope_allows(scope, permission_tool) then return false end
    if (permission_tool == "file_read" or permission_tool == "file_write") and workspace.is_sensitive_path(target) then
        return false
    end
    if not scope.workdir_only then return true end
    if permission_tool == "shell" then
        local workspace_root = workspace.configured_workspace_root()
        return type(workspace_root) == "string" and workspace_root ~= "" and target == workspace_root
    end
    if permission_tool == "file_read" or permission_tool == "file_write" then
        return path_is_within_workspace(target)
    end
    return false
end

-- Snapshot construction never asks for additional access. Native read repeats
-- the persistent permission check; run scope cannot override an explicit deny.
function M.review_read_allowed(path, scope)
    if workspace.is_sensitive_path(path) then return false end
    if not path_is_within_workspace(path) then return false end
    local permission=permit.check('file_read',path)
    return permission ~= 'deny' and (permission == 'allow' or
        scope_allows_target(scope, 'file_read', path))
end

local function apply_prompt_decision(decision, scope, permission_tool, target)
    if decision == "allow_session" or decision == "always" then
        if scope and permission_tool then
            if type(scope.allowed_targets) ~= "table" then scope.allowed_targets = {} end
            if type(scope.allowed_targets[permission_tool]) ~= "table" then
                scope.allowed_targets[permission_tool] = {}
            end
            scope.allowed_targets[permission_tool][tostring(target)] = true
        end
        return true
    end
    if decision == "allow_tool_run" then
        if scope and permission_tool then
            if type(scope.allowed_tools) ~= "table" then scope.allowed_tools = {} end
            scope.allowed_tools[permission_tool] = true
        end
        return true
    end
    if decision == "allow_run" then
        if scope then scope.full_control = true end
        return true
    end
    return decision == "allow"
end

prompt_decision_allows = function(decision)
    return decision == "allow" or decision == "allow_session" or
        decision == "allow_tool_run" or decision == "allow_run" or
        decision == "always"
end

local function should_persist_prompt_decision(tool_name, permission_tool, decision)
    return tool_name == "wiki_ingest" and permission_tool == "file_read" and
        prompt_decision_allows(decision)
end

-- Called only after every target has passed the permission checks.
local function tool_permission_context(permission_tool, target, tool_name, args)
    if not permission_tool then return nil end
    local ctx = {tool = permission_tool, target = target}
    if permission_tool == "file_read" or permission_tool == "file_write" then
        local targets = tool_name == "file_read" and workspace.file_read_paths(args) or {target}
        for _, path in ipairs(targets) do
            if not workspace.embedded_asset_name(path) and
                not path_is_within_workspace(normalize_permission_target(permission_tool, path)) then
                ctx.allow_outside_workspace = true
            end
        end
    end
    return ctx
end

local function mark_workspace_mutation(run_ctx, permission_tool, target, tool_ok)
    if not tool_ok or permission_tool ~= "file_write" then return end
    if run_ctx and type(run_ctx.state) == "table" then
        run_ctx.state.workspace_mutated = true
        run_ctx.state.successful_validation = false
        run_ctx.state.workspace_write_targets = run_ctx.state.workspace_write_targets or {}
        run_ctx.state.workspace_write_targets[tostring(target or "")] = true
    end
end

shell_command_is_validation = function(command)
    local standalone = {
        actionlint = true, cc = true, clang = true, clangpp = true, cpp = true,
        ctest = true, eslint = true, gcc = true, gpp = true, javac = true,
        jest = true, mypy = true, pytest = true, rspec = true, rustc = true,
        tsc = true, vitest = true,
    }
    local subcommands = {
        bun = {build = true, lint = true, test = true, typecheck = true},
        cargo = {build = true, check = true, clippy = true, test = true},
        cmake = {build = true},
        dotnet = {build = true, test = true},
        go = {build = true, test = true, vet = true},
        gradle = {build = true, check = true, test = true},
        make = {build = true, check = true, test = true},
        mvn = {test = true, verify = true},
        ninja = {test = true},
        npm = {build = true, lint = true, test = true, typecheck = true},
        pnpm = {build = true, lint = true, test = true, typecheck = true},
        yarn = {build = true, lint = true, test = true, typecheck = true},
    }

    for segment in tostring(command or ""):lower():gmatch("[^;&|]+") do
        local tokens = {}
        for token in segment:gmatch("%S+") do
            local clean = token:gsub("^[\"'(]+", ""):gsub("[\"',)]+$", "")
            table.insert(tokens, clean)
        end
        local index = 1
        while tokens[index] and tokens[index]:match("^[%w_]+=") do index = index + 1 end
        local executable = tokens[index] and (tokens[index]:match("([^/]+)$") or tokens[index]) or ""
        executable = executable:gsub("%+%+", "pp")
        if executable == "gradlew" then executable = "gradle" end
        if standalone[executable] then return true end
        if executable:match("^test[%w_.-]*$") or executable:match("^run[_-]?tests?[%w_.-]*$") then
            return true
        end
        if (executable == "python" or executable == "python3") and
           tokens[index + 1] == "-m" and standalone[tokens[index + 2] or ""] then
            return true
        end
        local accepted = subcommands[executable]
        if accepted then
            for next_index = index + 1, #tokens do
                local argument = tokens[next_index]:gsub("^%-+", "")
                argument = argument:match("([^/:=]+)$") or argument
                if accepted[argument] then return true end
            end
        end
    end
    return false
end

local function mark_validation(run_ctx, tool_name, args, tool_ok)
    if not tool_ok or tool_name ~= "shell" or (args and args.background == true) or not shell_command_is_validation(args and args.command) then return end
    if run_ctx and type(run_ctx.state) == "table" and run_ctx.state.workspace_mutated then
        run_ctx.state.successful_validation = true
    end
end

-- Processes tool_calls from an LLM response: rejects unavailable tools, decodes
-- arguments, checks permissions, executes handlers, appends results, then
-- recurses via continue_fn.
function M.handle_tool_calls(current_msgs, combined_tools, tool_calls, assistant_text, continue_fn, run_ctx)
    local reviewer=run_ctx and run_ctx.reviewer
    local controller=run_ctx and run_ctx.review_controller
    local hook_run={}
    for key,value in pairs(run_ctx or {}) do
        if key~='reviewer' and key~='review_controller' and key~='request_completion' then hook_run[key]=value end
    end
    if run_ctx and run_ctx.request_completion and #tool_calls==1 and
        tool_calls[1].name=='request_completion' and tool_available(combined_tools,'request_completion') then
        local args,err=decode_tool_arguments(tool_calls[1].arguments,run_ctx)
        if not err and type(args)=='table' and ({ready=true,question=true,blocked=true})[args.status] and
            type(args.text)=='string' and #args.text<=128*1024 and args.text:find('%S') and not args.text:find('%z') then
            local extra=false
            for key in pairs(args) do if key~='status' and key~='text' then extra=true end end
            if not extra then return run_ctx.request_completion(args.text,args.status) end
        end
    end
    logging.runtime_log("tools", string.format(
        "received %d tool call(s) assistant_text_bytes=%d",
        #tool_calls,
        #(assistant_text or "")
    ))
    local function append_status(text, effect)
        if run_ctx and run_ctx.silent_tools then return end
        ui.append(text, "agent", effect)
    end
    local openai_tool_calls = {}
    for _, tc in ipairs(tool_calls) do
        tc.arguments = sanitize_tool_arguments(tc.arguments, run_ctx)
        table.insert(openai_tool_calls, {
            id = tc.id,
            type = "function",
            ["function"] = {
                name = tc.name,
                arguments = redacted_tool_arguments(tc.name, tc.arguments),
            }
        })
    end

    local assistant_message = {
        role = "assistant",
        content = (assistant_text ~= "" and assistant_text or nil),
        tool_calls = openai_tool_calls
    }
    if run_ctx and type(run_ctx.assistant_reasoning_details) == "table" and
       #run_ctx.assistant_reasoning_details > 0 then
        assistant_message.reasoning_details = run_ctx.assistant_reasoning_details
    elseif run_ctx and type(run_ctx.assistant_reasoning) == "string" and
           run_ctx.assistant_reasoning ~= "" then
        local field = run_ctx.assistant_reasoning_field == "reasoning_content" and
            "reasoning_content" or "reasoning"
        assistant_message[field] = run_ctx.assistant_reasoning
    end
    table.insert(current_msgs, assistant_message)

    local pending_image_blocks = {}

    for _, tc in ipairs(tool_calls) do
        if run_ctx and run_ctx.is_cancelled and run_ctx.is_cancelled() then
            continue_fn(current_msgs, combined_tools)
            return
        end
        local result_content
        local event_ok = false
        local error_category
        local permission_wait_ms = 0
        local tool_started_at = nil
        local callbacks = run_ctx and run_ctx.callbacks or nil
        local event_tool_call = {
            id = tc.id,
            name = tc.name,
            arguments = tc.arguments,
            original_name = tc.name,
            original_arguments = tc.arguments,
        }
        local event_started = false
        local observer_aborted = false
        local stop_after_event = false

        local tool_span
        local function observer_failed(name, observer_error)
            telemetry.finish(tool_span, false, false, {["error.category"] = "observer"})
            local message = "observability callback " .. name ..
                " failed: " .. tostring(observer_error)
            logging.runtime_log("tool_event", message, "error")
            observer_aborted = true
            if run_ctx and type(run_ctx.stop_run) == "function" then
                run_ctx.stop_run(message, current_msgs, "observer")
            end
            return false
        end

        local function start_tool_event(name, arguments)
            if run_ctx and ((run_ctx.telemetry_context and run_ctx.telemetry_context.ended) or
                (run_ctx.is_cancelled and run_ctx.is_cancelled())) then return false end
            if event_started then return true end
            event_started = true
            event_tool_call.name = name or tc.name
            if arguments ~= nil then
                event_tool_call.effective_arguments = arguments
            end
            tool_started_at = now_ms()
            tool_span = telemetry.start("agent.tool", run_ctx and run_ctx.telemetry_context,
                telemetry.tool_attributes(event_tool_call.name, arguments, {
                    operation = "tool", tool = event_tool_call.name,
                    depth = run_ctx and run_ctx.depth,
                }))
            if run_ctx then run_ctx.telemetry_tool = tool_span end
            if callbacks and type(callbacks.on_tool_start) == "function" then
                local ok, observer_error = pcall(
                    callbacks.on_tool_start, event_tool_call)
                if not ok then
                    return observer_failed("on_tool_start", observer_error)
                end
            end
            return true
        end

        local event_finished = false
        local function finish_tool_event()
            if event_finished then return true end
            if not event_started and not start_tool_event(tc.name) then
                return false
            end
            event_finished = true
            local cancelled = run_ctx and run_ctx.is_cancelled and run_ctx.is_cancelled() or false
            local tool_duration_ms = math.max(0, math.floor(now_ms() - tool_started_at))
            telemetry.finish(tool_span, event_ok and not cancelled, cancelled, {
                ["error.category"] = error_category,
                duration_ms = tool_duration_ms,
            })
            if run_ctx then run_ctx.telemetry_tool = nil end
            if callbacks and type(callbacks.on_tool_done) == "function" then
                local ok, observer_error = pcall(
                    callbacks.on_tool_done,
                    event_tool_call, tool_result_text(result_content), event_ok,
                    tool_duration_ms,
                    permission_wait_ms)
                if not ok then
                    return observer_failed("on_tool_done", observer_error)
                end
            end
            return true
        end

        local body_ok, body_error = xpcall(function()
        if not tool_available(combined_tools, tc.name) then
            if not start_tool_event(tc.name) then return end
            error_category = "tool_unavailable"
            result_content = "Tool " .. tostring(tc.name) .. " is not available in the active profile"
            logging.runtime_log("tool", string.format("unavailable name=%s", tostring(tc.name)))
            append_status(string.format("\n\n⚙ %s: unavailable — denied\n\n", tostring(tc.name)))
        else
            local args, decode_err = decode_tool_arguments(tc.arguments, run_ctx)

            if decode_err then
                if not start_tool_event(tc.name) then return end
                error_category = "invalid_arguments"
                result_content = decode_err
                logging.runtime_log("tool", string.format("invalid_args name=%s error=%s", tc.name, logging.compact(decode_err, 240)))
                append_status(string.format("\n\n⚙ %s: invalid arguments — error: %s\n\n", tc.name, logging.compact(decode_err, 160)))
            else
                local target = tool_call_target(tc.name, args)
                local permission_tool = tool_permission_name(tc.name, run_ctx)
                local call_ctx = hooks.run("before_tool_call", {
                    name = tc.name,
                    args = args,
                    target = target,
                    permission_tool = permission_tool,
                    raw_arguments = tc.arguments,
                    tool_call = tc,
                    run = hook_run,
                })
                local tool_name = call_ctx.name or tc.name
                args = call_ctx.args or args
                target = call_ctx.target or target
                permission_tool = call_ctx.permission_tool or permission_tool

                local routed
                tool_name, args, routed = route_internal_wiki_read(tool_name, args)
                if routed then
                    target = tool_call_target(tool_name, args)
                    permission_tool = tool_permission_name(tool_name, run_ctx)
                end
                if not start_tool_event(tool_name, args) then return end

                if reviewer then
                    -- Enforce AFTER hooks/routing. Never invoke a plugin, MCP, shell,
                    -- permission prompt or live file adapter inside a reviewer.
                    if tool_name~='file_read' or not tool_available(combined_tools,tool_name) then
                        error_category='permission'; result_content='Reviewer tools are read-only snapshot adapters'
                    else
                        local text,err,category=reviewer.snapshot:read(args)
                        event_ok=text~=nil
                        local recoverable=category=='size' or category=='invalid_arguments'
                        result_content=text or ((recoverable and 'Snapshot read retry required: ' or
                            'Snapshot access denied: ')..tostring(err))
                        if not event_ok then error_category=recoverable and category or 'permission' end
                    end
                elseif require('agent.review_snapshot').blocked(workspace.configured_workspace_root()) and
                    not ({file_read=true,logs=true,processes=true,vcs=true,issues=true,tasks=true})[tool_name] then
                    error_category='permission'; result_content='Completion review write barrier: managed operation deferred'
                elseif not tool_available(combined_tools, tool_name) then
                    error_category = "tool_unavailable"
                    result_content = "Tool " .. tostring(tool_name) .. " is not available in the active profile"
                    logging.runtime_log("tool", string.format("unavailable name=%s", tostring(tool_name)))
                    append_status(string.format("\n\n⚙ %s: unavailable — denied\n\n", tostring(tool_name)))
                else
                    if tool_name == "subagents" then
                        permission_tool = nil
                    end
                    if permission_tool then
                        target = normalize_permission_target(permission_tool, target)
                    end
                    local guard_error = guard_before_tool(tool_name, args, run_ctx)
                    if guard_error then
                        local guard_span = telemetry.start("operation", tool_span, {
                            operation = "guard_stop", tool = tool_name,
                        })
                        error_category = "guard"
                        telemetry.finish(guard_span, false, false, {["error.category"] = error_category})
                        result_content = guard_error
                        stop_after_event = true
                        return
                    end
                    local display_target = tool_display_target(tool_name, args, target)
                    local display_command = tool_display_command(tool_name, args)
                    local show_generic_status = tool_name ~= "subagents"
                    if tool_name == "shell" then
                        logging.runtime_log("tool", string.format("call name=%s target=%s display=%s command=%s args=%s", tool_name, target, display_target, redact_sensitive_text(display_command or ""), redacted_tool_arguments(tool_name, tc.arguments) or ""))
                    elseif display_command then
                        logging.runtime_log("tool", string.format("call name=%s target=%s display=%s command=%s args=%s", tool_name, target, display_target, redact_sensitive_text(display_command), redacted_tool_arguments(tool_name, tc.arguments) or ""))
                    else
                        logging.runtime_log("tool", string.format("call name=%s target=%s args=%s", tool_name, target, redacted_tool_arguments(tool_name, tc.arguments) or ""))
                    end

                    do
                        local permission_scope = run_ctx and run_ctx.permission_scope or nil
                        local targets = tool_name == "file_read" and workspace.file_read_paths(args) or {target}
                        local authorized = true
                        if show_generic_status then
                            append_status(tool_status_prefix(tool_name, display_target, display_command))
                        end
                        -- Check every disk target before reading any part of a batch.
                        for _, requested_target in ipairs(targets) do
                            if not permission_tool or (tool_name == "file_read" and
                                workspace.embedded_asset_name(requested_target)) then goto next_target end
                            local target = normalize_permission_target(permission_tool, requested_target)
                            local perm = "allow"
                            local explicit_allow = false
                            local shell_scope_ok = true
                            local shell_scope_reason = nil
                            if permission_tool == "shell" and permission_scope and permission_scope.workdir_only then
                                shell_scope_ok, shell_scope_reason = workspace.shell_command_within_workspace(args.command)
                            end
                            if permission_scope and permission_scope.workdir_only and
                                (permission_tool == "file_read" or permission_tool == "file_write") and
                                not path_is_within_workspace(target) then
                                shell_scope_ok = false
                                shell_scope_reason = "file path escapes workspace: " .. target
                            end
                            if not shell_scope_ok then
                                perm = "deny"
                                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=deny reason=%s", permission_tool, tool_name, target, tostring(shell_scope_reason)))
                            else
                                perm, explicit_allow = permit.check(permission_tool, target)
                                if perm ~= "deny" and scope_allows_target(permission_scope, permission_tool, target) then
                                    perm = "allow"
                                    logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=allow scope=run", permission_tool, tool_name, target))
                                else
                                    if (permission_tool == "file_read" or permission_tool == "file_write") and workspace.is_sensitive_path(target) and perm == "allow" and not explicit_allow then
                                        perm = "ask"
                                    end
                                    logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=%s", permission_tool, tool_name, target, perm))
                                end
                            end

                            if perm ~= "ask" then
                                local decision_span = telemetry.start("operation", tool_span, {
                                    operation = "permission_decision", tool = permission_tool,
                                    purpose = perm == "allow" and "allow" or "deny",
                                })
                                telemetry.finish(decision_span, perm == "allow", false)
                            end
                            if perm == "deny" then
                                error_category = "permission"
                                authorized = false
                                result_content = shell_scope_reason or ("Permission denied for " .. tool_name .. " " .. target)
                                if show_generic_status then append_status(tool_status_suffix("— denied", display_command)) end
                            elseif perm == "ask" then
                                local decision, prompt_wait_ms, prompt_error = permission_prompt(run_ctx, permission_tool, target, {
                                    tool_name = tool_name,
                                    arguments = args,
                                    tool_call_id = tc.id,
                                })
                                permission_wait_ms = permission_wait_ms + prompt_wait_ms
                                if prompt_error ~= nil then error(prompt_error, 0) end
                                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s prompt=%s", permission_tool, tool_name, target, decision))
                                if decision == "deny" then
                                    error_category = "permission"
                                    authorized = false
                                    result_content = "User denied " .. tool_name .. " " .. target
                                    if show_generic_status then append_status(tool_status_suffix("— denied by user", display_command)) end
                                else
                                    if should_persist_prompt_decision(tool_name, permission_tool, decision) then
                                        permit.grant(permission_tool, target, true)
                                        permit.save()
                                    end
                                    authorized = apply_prompt_decision(decision, permission_scope, permission_tool, target)
                                    if not authorized then
                                        error_category = "permission"
                                        result_content = "Unknown permission decision for " .. tool_name .. ": " .. tostring(decision)
                                        if show_generic_status then append_status(tool_error_status(result_content, display_command), tool_name == "shell" and "shell" or nil) end
                                    end
                                end
                            end
                            if not authorized then break end
                            ::next_target::
                        end
                        if authorized then
                            if controller and controller.before_tool then
                                local proceed,why=controller:before_tool()
                                if not proceed then result_content=why; stop_after_event=true; return end
                            end
                            local tool_ok, error_summary
                            result_content, tool_ok, error_summary = execute_tool(tool_name, args, run_ctx,
                                        tool_permission_context(permission_tool, target, tool_name, args),
                                        display_command)
                            event_ok = tool_ok == true
                            if controller and controller.after_tool then
                                controller:after_tool(permission_tool=='file_write' or permission_tool=='shell' or permission_tool=='mcp')
                            end
                            if not event_ok then error_category = "tool" end
                            mark_workspace_mutation(run_ctx, permission_tool, target, tool_ok)
                            mark_validation(run_ctx, tool_name, args, tool_ok)
                            if tool_ok then
                                logging.runtime_log("tool", string.format("done name=%s target=%s bytes=%d images=%d", tool_name, target, #tool_result_text(result_content), #tool_result_images(result_content)))
                                if show_generic_status then append_status(tool_success_status(tool_name, result_content, display_command, args), tool_name == "shell" and "shell" or nil) end
                            else
                                logging.runtime_log("tool", string.format("error name=%s target=%s error=%s", tool_name, target, logging.compact(tool_result_text(result_content), 240)))
                                if show_generic_status then append_status(tool_error_status(error_summary or result_content, display_command), tool_name == "shell" and "shell" or nil) end
                            end
                        end
                    end
                    local result_ctx = hooks.run("after_tool_call", {
                        name = tool_name,
                        args = args,
                        target = target,
                        permission_tool = permission_tool,
                        result = result_content,
                        tool_call = tc,
                        run = hook_run,
                    })
                    result_content = result_ctx.result
                end
            end
        end
        end, function(err)
            return debug.traceback(tostring(err), 2)
        end)

        if observer_aborted then return end
        if not body_ok then
            event_ok = false
            error_category = "exception"
            result_content = "Tool " .. tostring(event_tool_call.name) ..
                " failed: " .. tostring(body_error)
        end
        if reviewer and not event_ok and error_category~='invalid_arguments' and error_category~='size' then
            reviewer.access_error=reviewer.access_error or result_content
        end
        if not finish_tool_event() then return end
        if not body_ok then error(body_error, 0) end
        if stop_after_event then
            if run_ctx and type(run_ctx.stop_run) == "function" then
                run_ctx.stop_run(result_content, current_msgs)
            else
                logging.runtime_log("tool_guard", logging.compact(result_content, 500))
            end
            return
        end

        if result_content then
            local result_text = tool_result_text(result_content)
            local bounded, truncated, invalid_bytes
            if reviewer and event_ok then
                -- Snapshot reads already enforce their byte budget. A second
                -- line/byte cut would invalidate the snapshot's coverage ledger.
                bounded,invalid_bytes=require('agent.utf8').sanitize(result_text)
                truncated=false
                if invalid_bytes>0 then
                    reviewer.access_error=reviewer.access_error or
                        'Snapshot evidence incomplete: invalid UTF-8 bytes replaced'
                end
            else
                bounded,truncated,invalid_bytes=tool_output.bound(result_text)
            end
            if invalid_bytes > 0 then
                logging.runtime_log("tool", string.format(
                    "replaced_invalid_utf8_bytes=%d name=%s",
                    invalid_bytes, tostring(tc.name)
                ), "warn")
            end
            if truncated then
                logging.runtime_log("tool", string.format(
                    "result_truncated name=%s original_bytes=%d",
                    tostring(tc.name), #result_text
                ), "warn")
            end
            table.insert(current_msgs, {
                role = "tool",
                tool_call_id = tc.id,
                content = bounded ~= "" and bounded or "[image attached]"
            })
            for _, image in ipairs(tool_result_images(result_content)) do
                if type(image) == "table" and type(image.data) == "string" and
                    type(image.mime_type) == "string" then
                    table.insert(pending_image_blocks, {
                        type = "image_url",
                        image_url = {
                            url = "data:" .. image.mime_type .. ";base64," .. image.data,
                            detail = "auto",
                        },
                    })
                end
            end
        end
    end

    if #pending_image_blocks > 0 then
        local content = {{
            type = "text",
            text = "Images returned by the preceding tool calls for visual inspection.",
        }}
        for _, block in ipairs(pending_image_blocks) do table.insert(content, block) end
        table.insert(current_msgs, {role = "user", content = content})
    end

    logging.runtime_log("tools", "continuing with tool results")
    continue_fn(current_msgs, combined_tools)
end

return M
