local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")

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
                                provider = {type = "string"},
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

local function now_ms()
    if _G.capstan and type(_G.capstan.now_ms) == "function" then
        return _G.capstan.now_ms()
    end
    return os.clock() * 1000
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

    agent.append(string.format("\n⚙ subagents: running %d/%d\n", max_concurrent, #args.tasks), "agent")
    for _, task in ipairs(args.tasks) do
        agent.append("  " .. task_label(task) .. "\n", "agent")
    end
    agent.append("\n", "agent")

    local function start_one(index)
        local task = args.tasks[index]
        local started_at = now_ms()
        local state = {
            index = index,
            task = task,
            done = false,
            text_chunks = {},
            result = make_subagent_result(task, index, started_at),
        }
        states[index] = state
        results[index] = state.result
        active = active + 1

        local ok, err = _G.capstan.agent.run({
            messages = {{role = "user", content = task.task}},
            provider = task.provider,
            model = task.model,
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
                state.done = true
                active = active - 1
                completed = completed + 1
                local finished_at = now_ms()
                state.result.ok = result and result.ok ~= false
                state.result.text = result and result.text or table.concat(state.text_chunks)
                state.result.error = result and result.error or state.result.error or ""
                state.result.turns = result and result.turns or 0
                state.result.started_at = result and result.started_at or started_at
                state.result.finished_at = result and result.finished_at or finished_at
                state.result.duration_ms = result and result.duration_ms or math.max(0, math.floor(finished_at - started_at))
            end,
        })
        if not ok then
            if not state.done then
                state.done = true
                active = active - 1
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

    while completed < #args.tasks do
        if not http or type(http.poll) ~= "function" then
            return "Subagents failed: http.poll is not available", false
        end
        http.poll()
        while next_index <= #args.tasks and active < max_concurrent do
            start_one(next_index)
            next_index = next_index + 1
        end
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
    })
    output = hook_ctx.result or output

    return json.encode(output), true
end

-- Dispatches a single tool call to its plugin handler (or subagents builtin).
local function call_plugin_tool(tool_name, args, run_ctx)
    if tool_name == "subagents" then
        return run_subagents(args, run_ctx)
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
    local p = find_plugin_tool(tool_name)
    if p and p.tool and p.tool.permission and p.tool.permission ~= "" then
        return p.tool.permission
    end
    return tool_name
end

-- Parses the JSON-encoded arguments string from an LLM tool call.
local function decode_tool_arguments(raw)
    local ok, decoded = pcall(json.decode, raw or "{}")
    if not ok then
        return nil, "Invalid JSON arguments: " .. tostring(decoded)
    end
    if type(decoded) ~= "table" then
        return nil, "Invalid tool arguments: expected object"
    end
    return decoded, nil
end

local function is_absolute_path(path)
    return type(path) == "string" and path:sub(1, 1) == "/"
end

local function configured_workdir()
    if _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir ~= "" then
        return _G.capstan.workdir
    end
    return nil
end

local function expand_home_path(path)
    if type(path) ~= "string" then return path end
    if path ~= "~" and path:sub(1, 2) ~= "~/" then return path end
    local home = os.getenv("HOME")
    if not home or home == "" then return path end
    return home .. path:sub(2)
end

local function normalize_path(path)
    if type(path) ~= "string" or path == "" then
        return path
    end

    path = expand_home_path(path)
    if not is_absolute_path(path) then
        local workdir = configured_workdir()
        if not workdir or workdir == "" then
            return path
        end
        path = workdir:gsub("/+$", "") .. "/" .. path
    end

    local parts = {}
    for part in path:gmatch("[^/]+") do
        if part == "." then
        elseif part == ".." then
            if #parts > 0 then
                table.remove(parts)
            end
        else
            table.insert(parts, part)
        end
    end

    return "/" .. table.concat(parts, "/")
end

local function tool_call_target(tool_name, args)
    if tool_name == "shell" and _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir ~= "" then
        return _G.capstan.workdir
    end
    return args.command or args.path or args.url or args.uri or tool_name
end

local function normalize_permission_target(permission_tool, target)
    if permission_tool == "file_read" or permission_tool == "file_write" then
        return normalize_path(target)
    end
    return target
end

local function collapse_home_path(path)
    if type(path) ~= "string" or path == "" then return path end
    local home = os.getenv("HOME")
    if not home or home == "" then return path end
    home = home:gsub("/+$", "")
    if path == home then return "~" end
    if path:sub(1, #home + 1) == home .. "/" then
        return "~" .. path:sub(#home + 1)
    end
    return path
end

local function tool_display_target(tool_name, args, target)
    if tool_name == "shell" then
        return collapse_home_path(target)
    end
    return target
end

local function tool_display_command(tool_name, args)
    if tool_name ~= "shell" then return nil end
    local command = args and args.command
    if type(command) ~= "string" or command == "" then return nil end
    return command
end

local function tool_status_suffix(status, display_command)
    if display_command then
        return status .. "\n  $ " .. display_command .. "\n\n"
    end
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
    if scope.full_control then return true end
    return type(scope.allowed_tools) == "table" and scope.allowed_tools[permission_tool] == true
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
        table.insert(openai_tool_calls, {
            id = tc.id,
            type = "function",
            ["function"] = {name = tc.name, arguments = tc.arguments}
        })
    end

    table.insert(current_msgs, {
        role = "assistant",
        content = (assistant_text ~= "" and assistant_text or nil),
        tool_calls = openai_tool_calls
    })

    for _, tc in ipairs(tool_calls) do
        local result_content
        local args, decode_err = decode_tool_arguments(tc.arguments)

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
            local display_target = tool_display_target(tool_name, args, target)
            local display_command = tool_display_command(tool_name, args)
            local show_generic_status = tool_name ~= "subagents"
            if display_command then
                logging.runtime_log("tool", string.format("call name=%s target=%s display=%s command=%s args=%s", tool_name, target, display_target, logging.compact(display_command, 240), tc.arguments or ""))
            else
                logging.runtime_log("tool", string.format("call name=%s target=%s args=%s", tool_name, target, tc.arguments or ""))
            end
            local permission_scope = run_ctx and run_ctx.permission_scope or nil
            local perm = "allow"
            if scope_allows(permission_scope, permission_tool) then
                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=allow scope=run", permission_tool, tool_name, target))
            elseif permission_tool then
                perm = permit.check(permission_tool, target)
                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=%s", permission_tool, tool_name, target, perm))
            end

            if show_generic_status then
                append_status(string.format("\n⚙ %s: %s ", tool_name, display_target))
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
                        result_content, tool_ok = call_plugin_tool(tool_name, args, run_ctx)
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
                result_content, tool_ok = call_plugin_tool(tool_name, args, run_ctx)
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
