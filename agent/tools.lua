local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local mcp_client = require("agent.mcp")
local workspace = require("agent.workspace")

local M = {}

local function config_table(name)
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
            description = "Run multiple focused internal sub-agents in parallel and return their independent findings for orchestration.",
            parameters = {
                type = "object",
                properties = {
                    tasks = {
                        type = "array",
                        items = {
                            type = "object",
                            properties = {
                                id = {type = "string"},
                                task = {type = "string"},
                                model = {type = "string"},
                                max_turns = {type = "integer"},
                                tools = {
                                    type = "array",
                                    items = {type = "string"},
                                },
                            },
                            required = {"id", "task"},
                        },
                    },
                    max_concurrent = {type = "integer"},
                },
                required = {"tasks"},
            },
        },
    }
end

-- Gathers all available LLM tools: plugin tools + optional subagents tool.
function M.collect(opts)
    opts = opts or {}
    local tools = {}
    if _G.plugins then
        for _, p in pairs(_G.plugins) do
            if p.tool then
                table.insert(tools, {
                    type = "function",
                    ["function"] = {
                        name = p.tool.name,
                        description = p.tool.description,
                        parameters = p.tool.parameters,
                    }
                })
            end
        end
    end
    if capability_enabled("subagents") and not opts.disable_subagents then
        table.insert(tools, subagents_tool())
    end

    -- MCP tools (collected from connected servers, empty if not configured)
    for _, t in ipairs(mcp_client.collect_tools()) do
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

local function find_plugin_tool(tool_name)
    if tool_name == "subagents" and capability_enabled("subagents") then
        return {tool = {name = "subagents"}}
    end
    if not _G.plugins then return nil end
    for _, p in pairs(_G.plugins) do
        if p.tool and p.tool.name == tool_name then
            return p
        end
    end
    return nil
end

local function subagent_config_number(field, default)
    local configured = config_table("subagents")
    local value = configured and tonumber(configured[field]) or nil
    if not value or value <= 0 then return default end
    return value
end

local function subagent_max_turns(args)
    local max_turns = tonumber(args.max_turns)
    if not max_turns or max_turns <= 0 then
        max_turns = subagent_config_number("max_turns", 6)
    end
    local hard_cap = subagent_config_number("max_turns_cap", 200)
    if hard_cap and hard_cap > 0 and max_turns > hard_cap then
        max_turns = hard_cap
    end
    return max_turns
end

local function subagent_max_concurrent(args)
    local max_concurrent = tonumber(args.max_concurrent)
    if not max_concurrent or max_concurrent <= 0 then
        max_concurrent = subagent_config_number("max_concurrent", 3)
    end
    local cap = subagent_config_number("max_concurrent_cap", 8)
    if max_concurrent > cap then max_concurrent = cap end
    return math.max(1, math.floor(max_concurrent))
end

local function subagent_max_tasks()
    return math.max(1, math.floor(subagent_config_number("max_tasks", 8)))
end

local function subagent_max_attempts()
    return math.max(1, math.floor(subagent_config_number("max_attempts", 3)))
end

local function subagent_retryable_error(message)
    local text = tostring(message or "")
    local status = text:match("HTTP%s+(%d+)")
    if status then
        local code = tonumber(status)
        return code == 408 or code == 429 or code == 500 or code == 502 or code == 503 or code == 504
    end
    return text:match("^Connection error:") ~= nil
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
    if now_ms() - guard.started_at > guard.max_duration_ms then
        return string.format("agent run exceeded %ds", math.floor(guard.max_duration_ms / 1000))
    end
    return nil
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

local function task_label(task)
    local id = tostring(task.id or "task")
    local text = tostring(task.task or "")
    text = text:gsub("%s+", " "):match("^%s*(.-)%s*$")
    if #text > 72 then text = text:sub(1, 69) .. "..." end
    return id .. " - " .. text
end

local function filter_tools(parent_tools, requested)
    local inherited = {}
    for _, tool in ipairs(parent_tools or {}) do
        local name = tool["function"] and tool["function"].name
        if name and name ~= "subagents" then
            inherited[name] = tool
        end
    end
    if type(requested) ~= "table" or #requested == 0 then
        local result = {}
        for _, tool in ipairs(parent_tools or {}) do
            local name = tool["function"] and tool["function"].name
            if name and name ~= "subagents" then table.insert(result, tool) end
        end
        return result
    end
    local result = {}
    for _, name in ipairs(requested) do
        local tool = inherited[tostring(name)]
        if tool then table.insert(result, tool) end
    end
    return result
end

local function make_subagent_result(task, index, started_at)
    return {
        id = tostring(task.id or ("task_" .. tostring(index))),
        ok = false,
        text = "",
        error = "",
        turns = 0,
        started_at = started_at,
        finished_at = started_at,
        duration_ms = 0,
    }
end

local function provider_model_set(run_ctx, provider_name)
    local runtime = run_ctx and run_ctx.runtime
    if not runtime or type(runtime.list_models) ~= "function" then
        logging.runtime_log("subagents", "models unavailable: runtime has no list_models")
        return nil
    end
    local models, err = runtime.list_models(provider_name)
    if not models then
        logging.runtime_log("subagents", "models unavailable provider=" .. tostring(provider_name) .. " error=" .. tostring(err))
        return nil
    end
    local set = {}
    for _, model in ipairs(models) do
        if type(model) == "table" and type(model.id) == "string" and model.id ~= "" then
            set[model.id] = true
        end
    end
    return set
end

local function subagent_model(task, model_set, default_model)
    local requested = type(task.model) == "string" and task.model or ""
    if requested ~= "" and model_set and model_set[requested] then
        return requested
    end
    if requested ~= "" then
        logging.runtime_log("subagents", "ignored unavailable model=" .. requested)
    end
    return default_model
end

local function run_subagents(args, run_ctx)
    if type(args.tasks) ~= "table" or #args.tasks == 0 then
        return "Subagents failed: missing tasks", false
    end
    if not _G.capstan or not _G.capstan.agent or type(_G.capstan.agent.run) ~= "function" then
        return "Subagents failed: agent runtime is not available", false
    end
    local depth = tonumber(run_ctx and run_ctx.depth) or 0
    if depth >= 1 then
        return "Subagents denied: max depth reached", false
    end

    local max_tasks = subagent_max_tasks()
    if #args.tasks > max_tasks then
        return "Subagents failed: too many tasks (" .. tostring(#args.tasks) .. " > " .. tostring(max_tasks) .. ")", false
    end

    local max_concurrent = math.min(subagent_max_concurrent(args), #args.tasks)
    local provider_name = (run_ctx and run_ctx.provider_name) or nil
    local active_provider = run_ctx and run_ctx.provider or nil
    local default_model = active_provider and active_provider.model or nil
    local model_set = provider_model_set(run_ctx, provider_name)
    local parent_scope = run_ctx and run_ctx.permission_scope or nil
    local permission_scope = {
        allowed_tools = {},
        full_control = parent_scope and parent_scope.full_control or false,
    }
    local group_started_at = now_ms()
    local results = {}
    local states = {}
    local active = 0
    local next_index = 1
    local completed = 0
    local max_attempts = subagent_max_attempts()

    agent.append(string.format("\n⚙ subagents: running %d concurrent, %d total\n", max_concurrent, #args.tasks), "agent")
    for _, task in ipairs(args.tasks) do
        agent.append("  " .. task_label(task) .. "\n", "agent")
    end
    agent.append("\n", "agent")

    local function start_one(index, attempt)
        attempt = attempt or 1
        local task = args.tasks[index]
        local started_at = now_ms()
        local state = {
            index = index,
            attempt = attempt,
            task = task,
            done = false,
            text_chunks = {},
            result = make_subagent_result(task, index, started_at),
        }
        states[index] = state
        results[index] = state.result
        state.result.attempts = attempt
        active = active + 1
        local selected_model = subagent_model(task, model_set, default_model)

        logging.runtime_log("subagents", string.format(
            "start index=%d id=%s attempt=%d/%d provider=%s model=%s prompt=%s",
            index,
            state.result.id,
            attempt,
            max_attempts,
            tostring(provider_name or ""),
            tostring(selected_model or ""),
            logging.compact(task.task, 300)
        ))

        local function retry_if_allowed(error_message)
            if attempt >= max_attempts or not subagent_retryable_error(error_message) then
                return false
            end
            logging.runtime_log("subagents", string.format(
                "retry index=%d id=%s next_attempt=%d/%d error=%s",
                index,
                state.result.id,
                attempt + 1,
                max_attempts,
                logging.compact(error_message or "", 240)
            ))
            agent.append(string.format("  %s - retry %d/%d after %s\n",
                state.result.id,
                attempt + 1,
                max_attempts,
                tostring(error_message or "error")), "agent")
            start_one(index, attempt + 1)
            return true
        end

        local ok, err = _G.capstan.agent.run({
            messages = {{role = "user", content = task.task}},
            provider = provider_name,
            model = selected_model,
            max_turns = subagent_max_turns(task),
            depth = depth + 1,
            tools = filter_tools(run_ctx and run_ctx.tools, task.tools),
            silent_tools = true,
            update_status = false,
            update_usage = false,
            permission_scope = permission_scope,
        }, {
            on_text = function(chunk)
                table.insert(state.text_chunks, chunk)
            end,
            on_error = function(message)
                state.result.error = tostring(message or "")
            end,
            on_done = function(result)
                active = active - 1
                local finished_at = now_ms()
                local result_ok = result and result.ok ~= false
                local error_message = result and result.error or state.result.error or ""
                if not result_ok and retry_if_allowed(error_message) then
                    return
                end
                state.done = true
                completed = completed + 1
                state.result.ok = result_ok
                state.result.text = result and result.text or table.concat(state.text_chunks)
                state.result.error = error_message
                state.result.turns = result and result.turns or 0
                state.result.started_at = result and result.started_at or started_at
                state.result.finished_at = result and result.finished_at or finished_at
                state.result.duration_ms = result and result.duration_ms or math.max(0, math.floor(finished_at - started_at))
            end,
        })
        if not ok then
            active = active - 1
            if retry_if_allowed(err) then
                return
            end
            if not state.done then
                state.done = true
                completed = completed + 1
            end
            state.result.ok = false
            state.result.error = tostring(err)
            state.result.finished_at = now_ms()
            state.result.duration_ms = math.max(0, math.floor(state.result.finished_at - started_at))
        end
    end

    while next_index <= #args.tasks and active < max_concurrent do
        start_one(next_index)
        next_index = next_index + 1
    end

    if agent and type(agent.set_activity) == "function" then
        agent.set_activity("Delegating")
    end

    while completed < #args.tasks do
        if not http or type(http.poll) ~= "function" then
            if agent and type(agent.set_activity) == "function" then
                agent.set_activity(nil)
            end
            return "Subagents failed: http.poll is not available", false
        end
        http.poll()
        if type(http.wait_frame) == "function" then
            http.wait_frame()
        end
        while next_index <= #args.tasks and active < max_concurrent do
            start_one(next_index)
            next_index = next_index + 1
        end
    end

    if agent and type(agent.set_activity) == "function" then
        agent.set_activity(nil)
    end

    local done_count = 0
    local error_count = 0
    local total_turns = 0
    for _, result in ipairs(results) do
        total_turns = total_turns + (tonumber(result.turns) or 0)
        if result.ok then done_count = done_count + 1 else error_count = error_count + 1 end
    end
    local group_finished_at = now_ms()
    local group_duration_ms = math.max(0, math.floor(group_finished_at - group_started_at))

    agent.append(string.format("\n⚙ subagents: done %d/%d, error %d/%d, %.1fs\n",
        done_count, #results, error_count, #results, group_duration_ms / 1000.0), "agent")
    for _, result in ipairs(results) do
        local status = result.ok and "done" or "error"
        local turns_label = result.turns == 1 and "turn" or "turns"
        agent.append(string.format("  %s - %s, %d %s, %.1fs\n",
            result.id, status, result.turns or 0, turns_label, (result.duration_ms or 0) / 1000.0), "agent")
    end
    agent.append("\n", "agent")

    local output = {
        ok = error_count == 0,
        duration_ms = group_duration_ms,
        total_turns = total_turns,
        results = results,
    }

    local hook_ctx = hooks.run("after_subagents", {
        args = args,
        ok = output.ok,
        result = output,
        run = run_ctx or {},
    })
    output = hook_ctx.result or output

    return json.encode(output), true
end

-- Dispatches a single tool call to its plugin handler (or subagents builtin or MCP server).
local function call_plugin_tool(tool_name, args, run_ctx)
    if tool_name == "subagents" then
        return run_subagents(args, run_ctx)
    end

    -- MCP tool routing: names like "mcp__browser__browser_navigate"
    if mcp_client.is_mcp_tool(tool_name) then
        local result, ok = mcp_client.call(tool_name, args)
        return result, ok
    end

    local p = find_plugin_tool(tool_name)
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
    }
    function ctx:replace(ui_val, llm_val)
        return ui_val, llm_val or ui_val
    end
    local function traceback(err)
        return debug.traceback(tostring(err), 2)
    end
    local ok, ui_result, llm_result = xpcall(function()
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
    return llm_result or ui_result, true
end

local function tool_permission_name(tool_name)
    -- MCP tools use a shared "mcp" permission key
    if mcp_client.is_mcp_tool(tool_name) then
        return "mcp"
    end
    local p = find_plugin_tool(tool_name)
    if p and p.tool and p.tool.permission and p.tool.permission ~= "" then
        return p.tool.permission
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
    if tool_name == "shell" and _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir ~= "" then
        return _G.capstan.workdir
    end
    return args.command or args.path or args.url or args.uri or tool_name
end

local function normalize_permission_target(permission_tool, target)
    if permission_tool == "file_read" or permission_tool == "file_write" then
        return workspace.normalize_path(target, workspace.runtime_workdir())
    end
    return target
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

local function call_plugin_tool_redacted(tool_name, args, run_ctx)
    local result, ok = call_plugin_tool(tool_name, args, run_ctx)
    if tool_name == "shell" then
        result = redact_sensitive_text(result)
    end
    return result, ok
end

local function tool_display_target(tool_name, args, target)
    if tool_name == "shell" then
        return "shell"
    end
    return target
end

local function tool_display_command(tool_name, args)
    if tool_name == "shell" and args and type(args.command) == "string" and args.command ~= "" then
        return redact_sensitive_text(args.command)
    end
    return nil
end

local function tool_status_prefix(tool_name, display_target, display_command)
    if display_command then
        return string.format("\n⚙ %s\n  $ %s ", tool_name, display_command)
    end
    return string.format("\n⚙ %s: %s ", tool_name, display_target)
end

local function tool_status_suffix(status, _display_command)
    return status .. "\n\n"
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

local function path_is_within_workdir(path)
    local workdir = configured_workdir()
    if type(path) ~= "string" or path == "" or type(workdir) ~= "string" or workdir == "" then
        return false
    end
    local normalized_path = normalize_path(path):gsub("/+$", "")
    local normalized_workdir = normalize_path(workdir):gsub("/+$", "")
    return normalized_path == normalized_workdir or normalized_path:sub(1, #normalized_workdir + 1) == normalized_workdir .. "/"
end

local function scope_allows_target(scope, permission_tool, target)
    if not scope_allows(scope, permission_tool) then return false end
    if not scope or not scope.workdir_only then return true end
    if permission_tool == "shell" then
        local workdir = configured_workdir()
        return type(workdir) == "string" and workdir ~= "" and target == workdir
    end
    if permission_tool == "file_read" or permission_tool == "file_write" then
        return path_is_within_workdir(target)
    end
    return false
end

local function apply_prompt_decision(decision, scope, permission_tool)
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
    return decision == "allow" or decision == "always"
end

-- Processes tool_calls from an LLM response: decodes, checks permissions,
-- executes each via call_plugin_tool, appends results, then recurses via continue_fn.
function M.handle_tool_calls(current_msgs, combined_tools, tool_calls, assistant_text, continue_fn, run_ctx)
    logging.runtime_log("tools", string.format("received %d tool call(s)", #tool_calls))
    local function append_status(text)
        if run_ctx and run_ctx.silent_tools then return end
        agent.append(text, "agent")
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

    table.insert(current_msgs, {
        role = "assistant",
        content = (assistant_text ~= "" and assistant_text or nil),
        tool_calls = openai_tool_calls
    })

    for _, tc in ipairs(tool_calls) do
        local result_content
        local args, decode_err = decode_tool_arguments(tc.arguments, run_ctx)

        if decode_err then
            result_content = decode_err
            logging.runtime_log("tool", string.format("invalid_args name=%s error=%s", tc.name, logging.compact(decode_err, 240)))
            append_status(string.format("\n⚙ %s: invalid arguments — error\n\n", tc.name))
        else
            local target = tool_call_target(tc.name, args)
            local permission_tool = tool_permission_name(tc.name)
            local call_ctx = hooks.run("before_tool_call", {
                name = tc.name,
                args = args,
                target = target,
                permission_tool = permission_tool,
                raw_arguments = tc.arguments,
                tool_call = tc,
                run = run_ctx or {},
            })
            local tool_name = call_ctx.name or tc.name
            args = call_ctx.args or args
            target = call_ctx.target or target
            permission_tool = call_ctx.permission_tool or permission_tool
            if tool_name == "subagents" then
                permission_tool = nil
            end
            if permission_tool then
                target = normalize_permission_target(permission_tool, target)
            end
            local guard_error = guard_before_tool(tool_name, args, run_ctx)
            if guard_error then
                if run_ctx and type(run_ctx.stop_run) == "function" then
                    run_ctx.stop_run(guard_error, current_msgs)
                else
                    logging.runtime_log("tool_guard", logging.compact(guard_error, 500))
                end
                return
            end
            local display_target = tool_display_target(tool_name, args, target)
            local display_command = tool_display_command(tool_name, args)
            local show_generic_status = tool_name ~= "subagents"
            if tool_name == "shell" then
                logging.runtime_log("tool", string.format("call name=%s target=%s display=%s args=%s", tool_name, target, display_target, redacted_tool_arguments(tool_name, tc.arguments) or ""))
            elseif display_command then
                logging.runtime_log("tool", string.format("call name=%s target=%s display=%s command=%s args=%s", tool_name, target, display_target, logging.compact(redact_sensitive_text(display_command), 240), redacted_tool_arguments(tool_name, tc.arguments) or ""))
            else
                logging.runtime_log("tool", string.format("call name=%s target=%s args=%s", tool_name, target, redacted_tool_arguments(tool_name, tc.arguments) or ""))
            end
            local permission_scope = run_ctx and run_ctx.permission_scope or nil
            local perm = "allow"
            if scope_allows_target(permission_scope, permission_tool, target) then
                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=allow scope=run", permission_tool, tool_name, target))
            elseif permission_tool then
                perm = permit.check(permission_tool, target)
                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=%s", permission_tool, tool_name, target, perm))
            end

            if show_generic_status then
                append_status(tool_status_prefix(tool_name, display_target, display_command))
            end

            if perm == "deny" then
                result_content = "Permission denied for " .. tool_name .. " " .. target
                if show_generic_status then append_status(tool_status_suffix("— denied", display_command)) end
            elseif perm == "ask" then
                local decision = permit.prompt(permission_tool, target)
                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s prompt=%s", permission_tool, tool_name, target, decision))
                if decision == "deny" then
                    result_content = "User denied " .. tool_name .. " " .. target
                    if show_generic_status then append_status(tool_status_suffix("— denied by user", display_command)) end
                else
                    if decision == "always" then
                        permit.grant(permission_tool, target, true)
                        permit.save()
                    end
                    if not apply_prompt_decision(decision, permission_scope, permission_tool) then
                        result_content = "Unknown permission decision for " .. tool_name .. ": " .. tostring(decision)
                        if show_generic_status then append_status(tool_error_status(result_content, display_command)) end
                    else
                        local tool_ok
                        result_content, tool_ok = call_plugin_tool_redacted(tool_name, args, run_ctx)
                        if tool_ok then
                            logging.runtime_log("tool", string.format("done name=%s target=%s bytes=%d", tool_name, target, #(result_content or "")))
                            if show_generic_status then append_status(tool_status_suffix("— done", display_command)) end
                        else
                            logging.runtime_log("tool", string.format("error name=%s target=%s error=%s", tool_name, target, logging.compact(result_content or "", 240)))
                            if show_generic_status then append_status(tool_error_status(result_content, display_command)) end
                        end
                    end
                end
            else
                local tool_ok
                result_content, tool_ok = call_plugin_tool_redacted(tool_name, args, run_ctx)
                if tool_ok then
                    logging.runtime_log("tool", string.format("done name=%s target=%s bytes=%d", tool_name, target, #(result_content or "")))
                    if show_generic_status then append_status(tool_status_suffix("— done", display_command)) end
                else
                    logging.runtime_log("tool", string.format("error name=%s target=%s error=%s", tool_name, target, logging.compact(result_content or "", 240)))
                    if show_generic_status then append_status(tool_error_status(result_content, display_command)) end
                end
            end
            local result_ctx = hooks.run("after_tool_call", {
                name = tool_name,
                args = args,
                target = target,
                permission_tool = permission_tool,
                result = result_content,
                tool_call = tc,
                run = run_ctx or {},
            })
            result_content = result_ctx.result
        end

        if result_content then
            table.insert(current_msgs, {
                role = "tool",
                tool_call_id = tc.id,
                content = result_content
            })
        end
    end

    logging.runtime_log("tools", "continuing with tool results")
    continue_fn(current_msgs, combined_tools)
end

return M
