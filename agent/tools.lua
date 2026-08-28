local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local mcp_client = require("agent.mcp")
local workspace = require("agent.workspace")
local redact = require("agent.redact")
local utf8_sanitize = require("agent.utf8")
local ui = require("agent.ui")

local M = {}

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

local function subagent_config_number(field, default)
    local configured = config_table("subagents")
    local value = configured and tonumber(configured[field]) or nil
    if not value or value <= 0 then return default end
    return value
end

local function subagent_max_turns(args)
    local default_turns = subagent_config_number("max_turns", 6)
    local max_turns = tonumber(args.max_turns)
    if not max_turns or max_turns <= 0 then
        max_turns = default_turns
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

local function subagent_max_result_bytes()
    return math.max(256, math.floor(subagent_config_number("max_result_bytes", 16384)))
end

local function safe_subagent_error(message)
    return logging.safe_error(message or "subagent failed", 240)
end

local function bound_subagent_text(result, limit)
    local text = redact.text(tostring(result.text or ""))
    local original_bytes = tonumber(result.text_original_bytes) or #text
    local was_truncated = result.text_truncated == true
    if result.ok == false then
        result.text = ""
        result.text_truncated = nil
        result.text_original_bytes = nil
        return
    end
    local bounded, truncated = logging.truncate(text, limit,
        "\n...<subagent output truncated>")
    result.text = bounded
    if truncated then
        result.text_truncated = true
        result.text_original_bytes = original_bytes
    elseif was_truncated then
        result.text_truncated = true
        result.text_original_bytes = original_bytes
    else
        result.text_truncated = nil
        result.text_original_bytes = nil
    end
end

local function sanitize_subagent_result(result, max_result_bytes)
    if type(result) ~= "table" then return end
    result.id = logging.truncate(tostring(result.id or "task"), 128)
    result.error = result.error ~= "" and safe_subagent_error(result.error) or ""
    bound_subagent_text(result, max_result_bytes)
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

local function is_generated_output_inspection(args)
    local command = tostring(args and args.command or ""):lower()
    if command == "" then return false end
    local has_generated_path = command:find("dist/", 1, true) or
        command:find("build/", 1, true) or
        command:find("out/", 1, true) or
        command:find("coverage/", 1, true)
    if not has_generated_path then return false end

    local padded = " " .. command
    local inspectors = {
        " grep ", " rg ", " cat ", " sed ", " head ", " tail ",
        " wc ", " strings ", " find ", " ls ", " node -e ",
        " python -c ", " python3 -c ",
    }
    for _, needle in ipairs(inspectors) do
        if padded:find(needle, 1, true) then return true end
    end
    return false
end

local function generated_output_skip_reason(tool_name, args, run_ctx)
    if tool_name ~= "shell" or not is_generated_output_inspection(args) then
        return nil
    end
    local guard = run_ctx and run_ctx.guard
    if not guard then return nil end
    guard.generated_output_checks = (tonumber(guard.generated_output_checks) or 0) + 1
    local limit = tonumber(guard.max_generated_output_checks)
    if limit == nil or limit < 0 or guard.generated_output_checks <= limit then
        return nil
    end
    return "Skipped redundant generated-output inspection. A primary validation already has a static inspection; inspect source, run a direct behavioral check, or report the requirement as unverified instead of probing the same generated artifacts again."
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

local function permission_prompt(run_ctx, permission_tool, target, details)
    local guard = run_ctx and run_ctx.guard or nil
    if guard and type(guard.pause) == "function" then guard.pause() end
    local prompt_started_at = now_ms()
    local callbacks = run_ctx and run_ctx.callbacks or nil
    local prompt = callbacks and callbacks.on_permission_request or permit.prompt
    local ok, decision = pcall(prompt, permission_tool, target, details)
    local paused_ms = math.max(0, now_ms() - prompt_started_at)
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

local function task_label(task)
    local id = tostring(task.id or "task")
    local text = tostring(task.task or "")
    text = text:gsub("%s+", " "):match("^%s*(.-)%s*$")
    if #text > 72 then text = text:sub(1, 69) .. "..." end
    return id .. " - " .. text
end

local function append_prompt_part(parts, title, text)
    if type(text) ~= "string" then return end
    text = text:match("^%s*(.-)%s*$")
    if text == "" then return end
    table.insert(parts, title .. "\n" .. text)
end

local function subagent_prompt(args, task)
    local parts = {}
    append_prompt_part(parts, "Shared instructions from orchestrator:", args and args.instructions)
    append_prompt_part(parts, "Task-specific instructions:", task and task.instructions)
    append_prompt_part(parts, "Task:", task and task.task)
    return table.concat(parts, "\n\n")
end

local function inherited_tool_map(parent_tools)
    local inherited = {}
    for _, tool in ipairs(parent_tools or {}) do
        local name = tool["function"] and tool["function"].name
        if name and name ~= "subagents" then
            inherited[name] = tool
        end
    end
    return inherited
end

local function filter_tools(parent_tools, requested)
    local inherited = inherited_tool_map(parent_tools)
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

local function validate_subagent_tools(tasks, parent_tools)
    local inherited = inherited_tool_map(parent_tools)
    local errors = {}
    for index, task in ipairs(tasks or {}) do
        if type(task.tools) == "table" and #task.tools > 0 then
            local unknown = {}
            local seen = {}
            for _, requested in ipairs(task.tools) do
                local name = tostring(requested)
                if not inherited[name] and not seen[name] then
                    seen[name] = true
                    table.insert(unknown, name)
                end
            end
            if #unknown > 0 then
                table.sort(unknown)
                table.insert(errors, string.format(
                    "task %q requests unavailable tools: %s",
                    tostring(task.id or index), table.concat(unknown, ", ")
                ))
            end
        end
    end
    if #errors == 0 then return nil end

    local available = {}
    for name in pairs(inherited) do table.insert(available, name) end
    table.sort(available)
    local available_text = #available > 0 and table.concat(available, ", ") or "none"
    return "Subagents failed: " .. table.concat(errors, "; ") ..
        ". Available tools: " .. available_text
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

    local tool_error = validate_subagent_tools(args.tasks, run_ctx and run_ctx.tools)
    if tool_error then return tool_error, false end

    local max_concurrent = math.min(subagent_max_concurrent(args), #args.tasks)
    local provider_name = (run_ctx and run_ctx.provider_name) or nil
    local active_provider = run_ctx and run_ctx.provider or nil
    local default_model = active_provider and active_provider.model or nil
    local model_set = provider_model_set(run_ctx, provider_name)
    local parent_scope = run_ctx and run_ctx.permission_scope or nil
    local permission_scope = parent_scope or {
        allowed_tools = {},
        allowed_targets = {},
        full_control = false,
    }
    local group_started_at = now_ms()
    local results = {}
    local states = {}
    local active = 0
    local next_index = 1
    local completed = 0
    local max_attempts = subagent_max_attempts()
    local max_result_bytes = subagent_max_result_bytes()

    ui.append(string.format("\n\n⚙ subagents: running %d concurrent, %d total\n", max_concurrent, #args.tasks), "agent")
    for _, task in ipairs(args.tasks) do
        ui.append("  " .. task_label(task) .. "\n", "agent")
    end
    ui.append("\n", "agent")

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
        local child_tools = filter_tools(run_ctx and run_ctx.tools, task.tools)
        local child_max_turns = subagent_max_turns(task)

        logging.runtime_log("subagents", string.format(
            "start index=%d id=%s attempt=%d/%d provider=%s model=%s prompt=%s",
            index,
            state.result.id,
            attempt,
            max_attempts,
            tostring(provider_name or ""),
            tostring(selected_model or ""),
            logging.compact(subagent_prompt(args, task), 300)
        ))
        logging.runtime_log("subagents", string.format(
            "child index=%d id=%s depth=%d max_turns=%d tools=%d tool_names=%s",
            index,
            state.result.id,
            depth + 1,
            child_max_turns,
            #child_tools,
            M.names(child_tools)
        ))

        local function retry_if_allowed(error_message)
            local safe_error = safe_subagent_error(error_message)
            if attempt >= max_attempts or not subagent_retryable_error(safe_error) then
                return false
            end
            logging.runtime_log("subagents", string.format(
                "retry index=%d id=%s next_attempt=%d/%d error=%s",
                index,
                state.result.id,
                attempt + 1,
                max_attempts,
                safe_error
            ))
            ui.append(string.format("  %s - retry %d/%d after %s\n",
                state.result.id,
                attempt + 1,
                max_attempts,
                safe_error), "agent")
            start_one(index, attempt + 1)
            return true
        end

        local ok, err = _G.capstan.agent.run({
            messages = {{role = "user", content = subagent_prompt(args, task)}},
            provider = provider_name,
            model = selected_model,
            profile = run_ctx and run_ctx.profile or nil,
            max_turns = child_max_turns,
            depth = depth + 1,
            tools = child_tools,
            silent_tools = true,
            update_status = false,
            update_usage = false,
            permission_scope = permission_scope,
            mcp_scope = run_ctx and run_ctx.mcp_scope or nil,
        }, {
            on_permission_request = run_ctx and run_ctx.callbacks and
                run_ctx.callbacks.on_permission_request or nil,
            on_text = function(chunk)
                table.insert(state.text_chunks, chunk)
            end,
            on_error = function(message)
                state.result.error = safe_subagent_error(message)
            end,
            on_done = function(result)
                active = active - 1
                local finished_at = now_ms()
                local result_ok = result and result.ok ~= false
                local error_message = result and result.error or state.result.error or ""
                if error_message ~= "" then error_message = safe_subagent_error(error_message) end
                if not result_ok and retry_if_allowed(error_message) then
                    return
                end
                state.done = true
                completed = completed + 1
                state.result.ok = result_ok
                state.result.text = result_ok and
                    (result and result.text or table.concat(state.text_chunks)) or ""
                state.result.error = error_message
                state.result.turns = result and result.turns or 0
                state.result.started_at = result and result.started_at or started_at
                state.result.finished_at = result and result.finished_at or finished_at
                state.result.duration_ms = result and result.duration_ms or math.max(0, math.floor(finished_at - started_at))
                sanitize_subagent_result(state.result, max_result_bytes)
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
            state.result.error = safe_subagent_error(err)
            state.result.text = ""
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
        if type(http.wait_frame) == "function" then
            http.wait_frame()
        end
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

    ui.append(string.format("\n\n⚙ subagents: done %d/%d, error %d/%d, %.1fs\n",
        done_count, #results, error_count, #results, group_duration_ms / 1000.0), "agent")
    for _, result in ipairs(results) do
        local status = result.ok and "done" or "error"
        local turns_label = result.turns == 1 and "turn" or "turns"
        ui.append(string.format("  %s - %s, %d %s, %.1fs\n",
            result.id, status, result.turns or 0, turns_label, (result.duration_ms or 0) / 1000.0), "agent")
    end
    ui.append("\n", "agent")

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

    for _, result in ipairs(output.results or {}) do
        sanitize_subagent_result(result, max_result_bytes)
    end

    return json.encode(output), true
end

-- Dispatches a single tool call to its plugin handler (or subagents builtin or MCP server).
local function call_plugin_tool(tool_name, args, run_ctx, permission_ctx)
    if tool_name == "subagents" then
        return run_subagents(args, run_ctx)
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
    return llm_result or ui_result, result_ok ~= false
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
    return args.command or args.path or args.url or args.uri or tool_name
end

local function normalize_permission_target(permission_tool, target)
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
    if tool_name ~= "file_read" or type(args) ~= "table" then return tool_name, args, false end
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
    local result, ok = call_plugin_tool(tool_name, args, run_ctx, permission_ctx)
    if tool_name == "shell" then
        result = redact_sensitive_text(result)
    end
    return result, ok
end

local function tool_result_text(result)
    if type(result) == "table" then return tostring(result.text or "") end
    return tostring(result or "")
end

local function tool_output_limits()
    local configured = config_table("tool_output") or {}
    local max_bytes = tonumber(configured.max_bytes) or (50 * 1024)
    local max_lines = tonumber(configured.max_lines) or 2000
    return math.max(1024, math.floor(max_bytes)),
        math.max(1, math.floor(max_lines))
end

local function bound_tool_result(text)
    local sanitized, invalid_bytes = utf8_sanitize.sanitize(text)
    local max_bytes, max_lines = tool_output_limits()
    local line_count = 1
    local line_cut = nil
    for newline in sanitized:gmatch("()\n") do
        if line_count == max_lines then
            line_cut = newline
            break
        end
        line_count = line_count + 1
    end
    if not line_cut and #sanitized <= max_bytes then
        return sanitized, false, invalid_bytes
    end

    local original_bytes = #sanitized
    local candidate = line_cut and sanitized:sub(1, line_cut - 1) or sanitized
    local suffix = string.format(
        "\n\n[Tool output truncated: %d bytes; use a narrower query or a paged read.]",
        original_bytes
    )
    local prefix = logging.truncate(candidate, math.max(0, max_bytes - #suffix), "")
    return prefix .. suffix, true, invalid_bytes
end

local function tool_result_images(result)
    if type(result) ~= "table" or type(result.images) ~= "table" then return {} end
    return result.images
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

local shell_command_is_validation

local function tool_phase(tool_name, display_command)
    if tool_name == "file_read" or tool_name == "wiki_read" or
       tool_name == "wiki_source_read" then return "Reading" end
    if tool_name == "file_edit" or tool_name == "file_write" or
       tool_name == "wiki_ingest" then return "Editing" end
    if tool_name == "fetch" then return "Fetching" end
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

    local values = table.pack(xpcall(function()
        return call_plugin_tool_redacted(tool_name, args, run_ctx, permission_ctx)
    end, function(err)
        return debug.traceback(tostring(err), 2)
    end))

    if update_activity then agent.set_activity(nil) end
    if not values[1] then error(values[2], 0) end
    return table.unpack(values, 2, values.n)
end

local function tool_status_prefix(tool_name, display_target, display_command)
    local phase = tool_phase(tool_name, display_command)
    if display_command then
        return string.format("\n\n⚙ %s\n  %s\n  $ %s ", tool_name, phase, display_command)
    end
    return string.format("\n\n⚙ %s\n  %s: %s ", tool_name, phase, display_target)
end

local function tool_status_suffix(status, _display_command)
    return status .. "\n\n"
end

local function tool_success_status(tool_name, result_content, display_command)
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

local function prompt_decision_allows(decision)
    return decision == "allow" or decision == "allow_session" or
        decision == "allow_tool_run" or decision == "allow_run" or
        decision == "always"
end

local function should_persist_prompt_decision(tool_name, permission_tool, decision)
    return tool_name == "wiki_ingest" and permission_tool == "file_read" and
        prompt_decision_allows(decision)
end

local function tool_permission_context(permission_tool, target)
    if not permission_tool then return nil end
    local ctx = {tool = permission_tool, target = target}
    if permission_tool == "file_read" and not path_is_within_workspace(target) then
        ctx.allow_outside_workspace = true
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
    local normalized = " " .. tostring(command or ""):lower() .. " "
    local markers = {
        " test", " lint", " typecheck", " tsc ", " build", " check",
        " pytest", " vitest", " jest", " actionlint", " make test",
    }
    for _, marker in ipairs(markers) do
        if normalized:find(marker, 1, true) then return true end
    end
    return false
end

local function mark_validation(run_ctx, tool_name, args, tool_ok)
    if not tool_ok or tool_name ~= "shell" or not shell_command_is_validation(args and args.command) then return end
    if run_ctx and type(run_ctx.state) == "table" and run_ctx.state.workspace_mutated then
        run_ctx.state.successful_validation = true
    end
end

-- Processes tool_calls from an LLM response: rejects unavailable tools, decodes
-- arguments, checks permissions, executes handlers, appends results, then
-- recurses via continue_fn.
function M.handle_tool_calls(current_msgs, combined_tools, tool_calls, assistant_text, continue_fn, run_ctx)
    logging.runtime_log("tools", string.format(
        "received %d tool call(s) assistant_text_bytes=%d",
        #tool_calls,
        #(assistant_text or "")
    ))
    local function append_status(text)
        if run_ctx and run_ctx.silent_tools then return end
        ui.append(text, "agent")
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
        local result_content
        local event_ok = false
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

        local function observer_failed(name, observer_error)
            local message = "observability callback " .. name ..
                " failed: " .. tostring(observer_error)
            logging.runtime_log("tool_event", message, "error")
            observer_aborted = true
            if run_ctx and type(run_ctx.stop_run) == "function" then
                run_ctx.stop_run(message, current_msgs)
            end
            return false
        end

        local function start_tool_event(name, arguments)
            if event_started then return true end
            event_started = true
            event_tool_call.name = name or tc.name
            if arguments ~= nil then
                event_tool_call.effective_arguments = arguments
            end
            tool_started_at = now_ms()
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
            if callbacks and type(callbacks.on_tool_done) == "function" then
                local ok, observer_error = pcall(
                    callbacks.on_tool_done,
                    event_tool_call, tool_result_text(result_content), event_ok,
                    math.max(0, math.floor(now_ms() - tool_started_at)),
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
            result_content = "Tool " .. tostring(tc.name) .. " is not available in the active profile"
            logging.runtime_log("tool", string.format("unavailable name=%s", tostring(tc.name)))
            append_status(string.format("\n\n⚙ %s: unavailable — denied\n\n", tostring(tc.name)))
        else
            local args, decode_err = decode_tool_arguments(tc.arguments, run_ctx)

            if decode_err then
                if not start_tool_event(tc.name) then return end
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
                    run = run_ctx or {},
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

                if not tool_available(combined_tools, tool_name) then
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

                    local skip_reason = generated_output_skip_reason(tool_name, args, run_ctx)
                    if skip_reason then
                        result_content = skip_reason
                        logging.runtime_log("tool_guard", "generated_output_check_skipped command=" .. logging.compact(display_command or "", 300))
                        if show_generic_status then
                            append_status(tool_status_prefix(tool_name, display_target, display_command))
                            append_status(tool_status_suffix("— skipped redundant generated-output inspection", display_command))
                        end
                    else
                        local permission_scope = run_ctx and run_ctx.permission_scope or nil
                        local perm = "allow"
                        local explicit_allow = false
                        local shell_scope_ok = true
                        local shell_scope_reason = nil
                        if permission_tool == "shell" and permission_scope and permission_scope.workdir_only then
                            shell_scope_ok, shell_scope_reason = workspace.shell_command_within_workspace(args.command)
                        end
                        if not shell_scope_ok then
                            perm = "deny"
                            logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=deny reason=%s", permission_tool, tool_name, target, tostring(shell_scope_reason)))
                        elseif permission_tool then
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

                        if show_generic_status then
                            append_status(tool_status_prefix(tool_name, display_target, display_command))
                        end

                        if perm == "deny" then
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
                                result_content = "User denied " .. tool_name .. " " .. target
                                if show_generic_status then append_status(tool_status_suffix("— denied by user", display_command)) end
                            else
                                if should_persist_prompt_decision(tool_name, permission_tool, decision) then
                                    permit.grant(permission_tool, target, true)
                                    permit.save()
                                end
                                if not apply_prompt_decision(decision, permission_scope, permission_tool, target) then
                                    result_content = "Unknown permission decision for " .. tool_name .. ": " .. tostring(decision)
                                    if show_generic_status then append_status(tool_error_status(result_content, display_command)) end
                                else
                                    local tool_ok
                                    result_content, tool_ok = execute_tool(tool_name, args, run_ctx,
                                        tool_permission_context(permission_tool, target),
                                        display_command)
                                    event_ok = tool_ok == true
                                    mark_workspace_mutation(run_ctx, permission_tool, target, tool_ok)
                                    mark_validation(run_ctx, tool_name, args, tool_ok)
                                    if tool_ok then
                                        logging.runtime_log("tool", string.format("done name=%s target=%s bytes=%d images=%d", tool_name, target, #tool_result_text(result_content), #tool_result_images(result_content)))
                                        if show_generic_status then append_status(tool_success_status(tool_name, result_content, display_command)) end
                                    else
                                        logging.runtime_log("tool", string.format("error name=%s target=%s error=%s", tool_name, target, logging.compact(tool_result_text(result_content), 240)))
                                        if show_generic_status then append_status(tool_error_status(result_content, display_command)) end
                                    end
                                end
                            end
                        else
                            local tool_ok
                            result_content, tool_ok = execute_tool(tool_name, args, run_ctx,
                                        tool_permission_context(permission_tool, target),
                                        display_command)
                            event_ok = tool_ok == true
                            mark_workspace_mutation(run_ctx, permission_tool, target, tool_ok)
                            mark_validation(run_ctx, tool_name, args, tool_ok)
                            if tool_ok then
                                logging.runtime_log("tool", string.format("done name=%s target=%s bytes=%d images=%d", tool_name, target, #tool_result_text(result_content), #tool_result_images(result_content)))
                                if show_generic_status then append_status(tool_success_status(tool_name, result_content, display_command)) end
                            else
                                logging.runtime_log("tool", string.format("error name=%s target=%s error=%s", tool_name, target, logging.compact(tool_result_text(result_content), 240)))
                                if show_generic_status then append_status(tool_error_status(result_content, display_command)) end
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
                        run = run_ctx or {},
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
            result_content = "Tool " .. tostring(event_tool_call.name) ..
                " failed: " .. tostring(body_error)
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
            local bounded, truncated, invalid_bytes = bound_tool_result(result_text)
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
