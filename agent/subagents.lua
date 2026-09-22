local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local redact = require("agent.redact")
local ui = require("agent.ui")
local telemetry = require("agent.telemetry")
local workspace = require("agent.workspace")
---@type CapstanSubagentsApi
local M = {}
local function config_table(name)
    if _G.capstan and type(_G.capstan.runtime_options) == "table" and
       _G.capstan.runtime_options.isolated then return nil end
    if not _G.capstan or type(_G.capstan.config) ~= "table" then return nil end
    local value = _G.capstan.config[name]
    return type(value) == "table" and value or nil
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
    result.id = logging.truncate(redact.text(tostring(result.id or "task")), 128)
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


-- One scheduler owns foreground and detached work; callbacks only settle an
-- attempt. Dispatch/retry happens at ordinary poll boundaries, never recursively.
local groups, active, polling = {}, 0, false
-- Handles are runtime-only capabilities, never native IDs supplied by a model.
local handles = {}
local function copy(value, seen)
    if type(value) ~= "table" then return value end
    seen = seen or {}
    if seen[value] then return seen[value] end
    local out = {}; seen[value] = out
    for k, v in pairs(value) do out[k] = copy(v, seen) end
    return out
end
-- Bound encoded JSON, not just text bytes: escaping and aggregation can grow
-- far beyond per-child limits. Keep valid structured results in the C cache.
local function encode_bounded(value)
    local output = copy(value)
    local encoded = json.encode(output)
    while #encoded > 48 * 1024 do
        local largest
        for _, r in ipairs(output.results or {output}) do
            if type(r.text) == "string" and #r.text > 0 and
                (not largest or #r.text > #largest.text) then largest = r end
        end
        if not largest then
            return json.encode({ok = false, error = "Subagent result metadata exceeds output limit"})
        end
        local size = #largest.text
        largest.text_original_bytes = largest.text_original_bytes or size
        largest.text_truncated = true
        largest.text = logging.truncate(largest.text, math.floor(size / 2))
        encoded = json.encode(output)
    end
    return encoded
end
local function publish(g, state)
    if not g.background then return true end
    local r = state and state.result
    local id = state and state.native_id or g.id
    if not id then return true end
    local ok, err = pcall(function()
        local accepted = tools.background_update(id, {
            status = state and state.status or g.status,
            ok = (state and r.ok == true) or (not state and g.output and g.output.ok == true) or false,
            output = encode_bounded(state and r or g.output or {results = g.results}),
        })
        if accepted == false then error("background publication rejected") end
    end)
    if not ok then
        g.publication_error = safe_subagent_error(err)
        logging.runtime_log("subagents", "snapshot publication failed: " .. g.publication_error)
        return false
    end
    if state then state.published = state.status end
    return true
end
local function finish_group(g)
    if g.done then return end
    local turns, errors = 0, 0
    for _, s in ipairs(g.states) do
        if not s.done then return end
        turns = turns + (tonumber(s.result.turns) or 0)
        if not s.result.ok then errors = errors + 1 end
    end
    -- Retry failed terminal publications at poll boundaries. Never rerun hooks
    -- or claim completion while the native facade still reports active work.
    for _, s in ipairs(g.states) do
        if g.background and s.published ~= s.status and not publish(g, s) then return end
    end
    if g.output then
        if publish(g) then g.done = true end
        return
    end
    local output = {ok = errors == 0, results = copy(g.results), total_turns = turns,
        duration_ms = math.max(0, math.floor(now_ms() - g.started_at))}
    local ok, ctx = pcall(hooks.run, "after_subagents", {
        args = g.args, ok = output.ok, result = copy(output), run = g.ctx,
    })
    if g.reviewer then
        -- Internal verdicts are a protocol; plugin result rewriting cannot forge acceptance.
        if not ok then output.ok=false; output.error=safe_subagent_error(ctx) end
    elseif ok and type(ctx) == "table" and type(ctx.result) == "table" then
        -- Hooks may amend task findings, but cannot bypass registry/output bounds.
        local hooked = ctx.result
        output.ok = hooked.ok == true
        for i, r in ipairs(output.results) do
            local h = type(hooked.results) == "table" and hooked.results[i]
            if type(h) == "table" then
                for _, key in ipairs({"text", "error", "ok"}) do
                    if h[key] ~= nil then r[key] = h[key] end
                end
            end
        end
    elseif not ok then
        output.ok = false
        output.error = safe_subagent_error(ctx)
    end
    for _, result in ipairs(output.results) do
        result.ok = result.ok == true
        sanitize_subagent_result(result, g.limit)
        if not result.ok then output.ok = false end
    end
    if g.publication_error then
        output.ok, output.error = false, "Snapshot publication failed: " .. g.publication_error
    end
    g.output = output
    g.status = g.cancelled and "cancelled" or (output.ok and "completed" or "failed")
    -- Individual terminal snapshots are immutable. Hooks amend the group's
    -- aggregated findings only, never rewrite already-published task outcomes.
    if not publish(g) then
        output.ok, output.error = false, "Snapshot publication failed: " .. g.publication_error
        if not g.cancelled then g.status = "failed" end
        return
    end
    g.done = true
    if not g.background then
        ui.append(string.format("\n\n⚙ subagents: done %d/%d, error %d/%d, %.1fs\n",
            #g.states - errors, #g.states, errors, #g.states, output.duration_ms / 1000), "agent")
        for _, s in ipairs(g.states) do
            local r = s.result
            ui.append(string.format("  %s - %s, %d %s, %.1fs\n", r.id,
                r.ok and "done" or "error", r.turns, r.turns == 1 and "turn" or "turns",
                r.duration_ms / 1000), "agent")
        end
        ui.append("\n", "agent")
    end
end
local function release(g, s)
    if s.status == "running" then active = active - 1; g.active = g.active - 1 end
end
local function close_queue(s, ok)
    if s.queue then
        telemetry.finish(s.queue.span, ok, not ok, {duration_ms = math.max(0, now_ms() - s.queue.started_at)})
        s.queue = nil
    end
end
local function cancel_state(g, s)
    if s.done then return end
    release(g, s)
    s.generation = s.generation + 1
    s.done, s.status = true, "cancelled"
    close_queue(s, false)
    local cancel = s.cancel; s.cancel = nil
    s.result.ok, s.result.text, s.result.error = false, "", "Subagent cancelled"
    s.result.finished_at = now_ms()
    s.result.duration_ms = math.max(0, math.floor(s.result.finished_at - s.result.started_at))
    if type(cancel) == "function" then
        local ok, err = pcall(cancel)
        if not ok then logging.runtime_log("subagents", safe_subagent_error(err)) end
    end
    publish(g, s)
end
local function cancel_group(g)
    if g.done then return end
    g.cancelled = true
    if g.output then
        g.status, g.output.ok = "cancelled", false
    end
    for _, s in ipairs(g.states) do cancel_state(g, s) end
    finish_group(g)
end
local function is_cancelled(g, s)
    if g.cancelled then return true end
    if g.lifetime then
        local ok, cancelled = pcall(g.lifetime)
        if not ok or cancelled then return true end
    end
    if g.background then
        return tools.background_cancelled(g.id) or (s and tools.background_cancelled(s.native_id))
    end
    return (g.ctx.telemetry_tool and g.ctx.telemetry_tool.ended) or
        (g.ctx.is_cancelled and g.ctx.is_cancelled())
end
local function start_one(g, s)
    if g.resolve_models then
        local resolve = g.resolve_models
        g.resolve_models = nil
        g.models = resolve()
    end
    close_queue(s, true)
    s.attempt = s.attempt + 1
    s.generation = s.generation + 1
    local generation = s.generation
    s.status = "running"; active = active + 1; g.active = g.active + 1
    g.status = "running"
    publish(g)
    local started = now_ms()
    local r = s.result
    r.started_at, r.attempts, r.error = started, s.attempt, ""
    local text, original = "", 0
    local settled = false
    local function live() return not settled and not s.done and s.generation == generation end
    local function done(result)
        if not live() then return end
        settled = true
        release(g, s); s.cancel = nil
        result = result or {ok = false, error = "subagent failed"}
        local err = result.error or r.error or ""
        if err ~= "" then err = safe_subagent_error(err) end
        if result.ok == false and not is_cancelled(g, s) and s.attempt < g.attempts and subagent_retryable_error(err) then
            s.status = "queued"
            s.queue = {started_at = now_ms(), span = telemetry.start("operation", g.telemetry_parent,
                {operation = "retry", purpose = "subagent_transient_error", attempt = s.attempt + 1, depth = g.depth + 1})}
            r.error = err
            if not g.background then ui.append(string.format("  %s - retry %d/%d after %s\n", r.id, s.attempt + 1, g.attempts, err), "agent") end
        else
            s.done = true
            r.ok = result.ok ~= false
            r.text = r.ok and (result.text or text) or ""
            if not result.text and original > #text then r.text_truncated, r.text_original_bytes = true, original end
            r.error, r.turns = err, tonumber(result.turns) or 0
            r.finished_at = result.finished_at or now_ms()
            r.duration_ms = result.duration_ms or math.max(0, math.floor(r.finished_at - started))
            sanitize_subagent_result(r, g.limit)
            s.status = r.ok and "completed" or "failed"
        end
        publish(g, s)
    end
    local child_tools = filter_tools(g.ctx.tools, s.task.tools)
    if g.reviewer then
        -- Reviewer-only schema; never mutate the parent/live file_read contract.
        child_tools=copy(child_tools)
        for _,tool in ipairs(child_tools) do
            if tool['function'].name=='file_read' then
                tool['function'].description='Read immutable snapshot files. Batch paths must omit offset and limit. For paging use one path, offset=0, limit=48000, then follow next_offset to EOF. Argument/size errors allow retry; finish every file in an oversized batch.'
                tool['function'].parameters={type='object',properties={
                    path={type='string'}, paths={type='array',items={type='string'},minItems=1},
                    offset={type='integer',minimum=0,description='Byte offset; only with one file.'},
                    limit={type='integer',minimum=4,maximum=48000,description='Page bytes; only with one file.'},
                },additionalProperties=false}
            end
        end
    end
    local model = subagent_model(s.task, g.models, g.default_model)
    logging.runtime_log("subagents", string.format("start index=%d id=%s attempt=%d/%d provider=%s model=%s prompt=%s",
        s.index, r.id, s.attempt, g.attempts, tostring(g.ctx.provider_name or ""), tostring(model or ""), logging.compact(subagent_prompt(g.args, s.task), 300)))
    local names = {}; for _, t in ipairs(child_tools) do names[#names + 1] = t["function"].name end; table.sort(names)
    logging.runtime_log("subagents", string.format("child index=%d id=%s depth=%d max_turns=%d tools=%d tool_names=%s",
        s.index, r.id, g.depth + 1, s.max_turns, #child_tools, table.concat(names, ",")))
    publish(g, s)
    local called, ok, err, cancel = pcall(g.run, {
        messages = {{role = "user", content = subagent_prompt(g.args, s.task)}},
        provider = g.ctx.provider_name, provider_snapshot = g.ctx.provider,
        model = model, profile = g.ctx.profile_snapshot or g.ctx.profile,
        max_turns = s.max_turns, depth = g.depth + 1,
        telemetry_parent = g.telemetry_parent, subagent_index = s.index,
        subagent_id = r.id, subagent_attempt = s.attempt,
        background = g.background, background_id = s.native_id,
        workdir = g.ctx.workdir, workspace_root = g.ctx.workspace_root,
        system_prompt = g.ctx.system_prompt, process_owner = g.ctx.process_owner,
        tools = child_tools, silent_tools = true, update_status = false, update_usage = false,
        reviewer = g.reviewer,
        review_budget = g.reviewer and g.reviewer.budget,
        permission_scope = g.ctx.permission_scope, mcp_scope = g.ctx.mcp_scope,
        is_cancelled = function() return s.done or is_cancelled(g, s) end,
    }, {
        on_permission_request = g.ctx.callbacks and g.ctx.callbacks.on_permission_request,
        on_text = function(chunk)
            if not live() then return end
            chunk = tostring(chunk or ""); original = original + #chunk
            -- Keep a bounded raw prefix and redact the whole prefix at publication,
            -- so credentials split across chunks cannot leak.
            if #text < g.limit then text = text .. chunk:sub(1, g.limit - #text) end
        end,
        on_error = function(message) if live() then r.error = safe_subagent_error(message) end end,
        on_done = done,
    })
    if not called then done({ok = false, error = ok})
    elseif not ok then done({ok = false, error = err})
    elseif live() then s.cancel = cancel end
end
local function reclaim(g)
    if not g.done then return false, "Subagents group is still active" end
    if not tools or type(tools.background_release) ~= "function" then
        return false, "Background release unavailable"
    end
    -- Clear each ID only after successful release, so a partial failure can be
    -- retried without touching a stale ID or dropping the owning Lua context.
    for _, s in ipairs(g.states) do
        if s.native_id then
            local ok, err = pcall(tools.background_release, s.native_id)
            if not ok or err == false then return false, safe_subagent_error(err) end
            s.native_id = nil
        end
    end
    if g.id then
        local ok, err = pcall(tools.background_release, g.id)
        if not ok or err == false then return false, safe_subagent_error(err) end
        g.id = nil
    end
    for i, entry in ipairs(groups) do
        if entry == g then table.remove(groups, i); break end
    end
    return true
end
function M.poll()
    if polling then return end
    polling = true
    local ok, err = xpcall(function()
        local global_cap = math.huge
        for _, g in ipairs(groups) do
            if not g.done then global_cap = math.min(global_cap, g.cap) end
        end
        for _, g in ipairs(groups) do
            if not g.done then
                if is_cancelled(g) then cancel_group(g) else
                    for _, s in ipairs(g.states) do
                        if not s.done and is_cancelled(g, s) then cancel_state(g, s) end
                    end
                    for _, s in ipairs(g.states) do
                        if s.status == "queued" and g.active < g.concurrent and active < global_cap then start_one(g, s) end
                    end
                    finish_group(g)
                end
            end
        end
        for i = #groups, 1, -1 do
            local g = groups[i]
            if g.discard and g.done then reclaim(g) end
        end
    end, debug.traceback)
    polling = false
    if not ok then error(err, 0) end
end
function M.cancel_owner(owner)
    for _, g in ipairs(groups) do if g.ctx.process_owner == owner then cancel_group(g) end end
end
function M.shutdown()
    for _, g in ipairs(groups) do cancel_group(g) end
    -- Public groups keep their inspection history. Internal owners disappear
    -- at shutdown, so reclaim their records and invalidate their handles.
    for handle, g in pairs(handles) do
        if reclaim(g) then handles[handle] = nil end
    end
end
local function run(args, run_ctx, internal)
    args, run_ctx = args or {}, run_ctx or {}
    if type(args.tasks) ~= "table" or #args.tasks == 0 then return "Subagents failed: missing tasks", false end
    if not _G.capstan or not capstan.agent or type(capstan.agent.run) ~= "function" then return "Subagents failed: agent runtime is not available", false end
    local depth = tonumber(run_ctx.depth) or 0
    if depth >= 1 then return "Subagents denied: max depth reached", false end
    if #args.tasks > subagent_max_tasks() then return string.format("Subagents failed: too many tasks (%d > %d)", #args.tasks, subagent_max_tasks()), false end
    for _, task in ipairs(args.tasks) do if type(task) ~= "table" then return "Subagents failed: invalid task", false end end
    local tool_error = validate_subagent_tools(args.tasks, run_ctx.tools)
    if tool_error then return tool_error, false end
    local background = args.background == true
    if background and (not _G.tools or type(tools.background_register) ~= "function" or type(tools.background_update) ~= "function" or type(tools.background_cancelled) ~= "function") then
        return "Subagents failed: background registry unavailable", false
    end
    if #groups >= 128 then return "Subagents failed: retained group limit reached (128)", false end
    local ctx = {}
    for _, key in ipairs({"provider_name", "provider", "profile", "profile_snapshot", "tools", "permission_scope", "mcp_scope", "process_owner", "workdir", "workspace_root", "system_prompt"}) do ctx[key] = copy(run_ctx[key]) end
    ctx.process_owner = ctx.process_owner or run_ctx.session_id or run_ctx.mcp_scope
    if background and not ctx.process_owner then return "Subagents failed: missing process owner", false end
    ctx.workdir = ctx.workdir or workspace.runtime_workdir() or workspace.configured_workdir()
    ctx.workspace_root = ctx.workspace_root or workspace.configured_workspace_root()
    ctx.system_prompt = ctx.system_prompt or _G.system_prompt or ""
    ctx.permission_scope = ctx.permission_scope or {allowed_tools = {}, allowed_targets = {}, full_control = false}
    if not background then
        ctx.permission_scope = run_ctx.permission_scope or ctx.permission_scope
        ctx.is_cancelled, ctx.telemetry_tool, ctx.callbacks = run_ctx.is_cancelled, run_ctx.telemetry_tool, run_ctx.callbacks
    end
    local g = {args = copy(args), ctx = ctx, depth = depth, background = background,
        run = capstan.agent.run,
        models = not background and provider_model_set(run_ctx, ctx.provider_name) or nil,
        default_model = ctx.provider and ctx.provider.model, concurrent = subagent_max_concurrent(args),
        cap = math.max(1, math.floor(subagent_config_number("max_concurrent_cap", 8))),
        limit = subagent_max_result_bytes(), attempts = subagent_max_attempts(),
        active = 0, states = {}, results = {}, started_at = now_ms(), status = "queued",
        telemetry_parent = internal and internal.telemetry_parent or (not background and run_ctx.telemetry_tool or nil),
        lifetime = internal and internal.is_cancelled,
        reviewer = internal and internal.reviewer,
        notify = not internal or internal.notify == true}
    if background then
        -- Model metadata lookup may use blocking HTTP; never do it in launch.
        for _, task in ipairs(args.tasks) do
            if type(task.model) == "string" and task.model ~= "" then
                g.resolve_models = function() return provider_model_set(run_ctx, ctx.provider_name) end
                break
            end
        end
    end
    local function register(kind, label)
        local id, err = tools.background_register({owner = ctx.process_owner, kind = kind,
            label = logging.truncate(redact.text(label), 240), workdir = ctx.workdir,
            notify = kind == "subagent_group" and g.notify or false})
        if not id then error(err or "background registration failed") end
        return id
    end
    local ok, err = pcall(function()
        if background then g.id = register("subagent_group", "Subagents (" .. #args.tasks .. ")") end
        for i, task in ipairs(g.args.tasks) do
            local s = {index = i, task = task, status = "queued", attempt = 0, generation = 0,
                -- Internal review inherits the resolved orchestrator budget,
                -- not the public subagent default/cap. Other limits still apply.
                max_turns = g.reviewer and task.max_turns or subagent_max_turns(task),
                result = make_subagent_result(task, i, g.started_at)}
            sanitize_subagent_result(s.result, g.limit)
            if background then s.native_id = register("subagent", task_label(task)) end
            g.states[i], g.results[i] = s, s.result
            s.queue = {started_at = now_ms(), span = telemetry.start("operation", g.telemetry_parent, {operation = "subagent_queue", depth = depth + 1})}
            if not publish(g, s) then error(g.publication_error) end
        end
        if background and not publish(g) then error(g.publication_error) end
    end)
    if not ok then
        -- Retain failed cleanup only until terminal publication/release can be
        -- retried. Never leak partially registered tasks on capacity failure.
        g.discard = true
        groups[#groups + 1] = g
        cancel_group(g)
        reclaim(g)
        return "Subagents failed: " .. safe_subagent_error(err), false
    end
    groups[#groups + 1] = g
    if internal then
        local handle = {}
        handles[handle] = g
        return handle, true
    end
    if background then
        local ids = {}; for _, s in ipairs(g.states) do ids[#ids + 1] = {id = s.result.id, background_id = s.native_id, status = "queued"} end
        return json.encode({background = true, started = true, id = g.id, group_id = g.id, status = "queued", tasks = ids}), true
    end
    ui.append(string.format("\n\n⚙ subagents: running %d concurrent, %d total\n", math.min(g.concurrent, #args.tasks), #args.tasks), "agent")
    for _, task in ipairs(args.tasks) do ui.append("  " .. task_label(task) .. "\n", "agent") end
    ui.append("\n", "agent")
    while not g.done do
        M.poll()
        if not g.done and active > 0 then
            if not _G.http or type(http.poll) ~= "function" then
                cancel_group(g)
                for i, entry in ipairs(groups) do if entry == g then table.remove(groups, i); break end end
                return "Subagents failed: http.poll is not available", false
            end
            http.poll()
            if type(http.wait_frame) == "function" then http.wait_frame() end
        end
    end
    for i, entry in ipairs(groups) do if entry == g then table.remove(groups, i); break end end
    if g.cancelled then return "Subagents cancelled", false end
    return json.encode(g.output), true
end

function M.run(args, run_ctx)
    return run(args, run_ctx)
end

-- Internal asynchronous adapter. Public model arguments cannot supply lifetime
-- predicates, telemetry parents, handles or notification policy.
function M.submit(args, run_ctx, options)
    options = options or {}
    if type(options) ~= "table" or
        (options.is_cancelled ~= nil and type(options.is_cancelled) ~= "function") or
        (options.notify ~= nil and type(options.notify) ~= "boolean") then
        return nil, "Invalid internal subagent options"
    end
    local capabilities = config_table("capabilities")
    if capabilities and capabilities.subagents == false then
        return nil, "Subagents denied: capability disabled"
    end
    if not tools or type(tools.background_release) ~= "function" then
        return nil, "Subagents failed: background release unavailable"
    end
    if args ~= nil and type(args) ~= "table" then return nil, "Subagents failed: invalid arguments" end
    args = copy(args or {})
    args.background = true
    local handle, ok = run(args, run_ctx, options)
    if not ok then return nil, handle end
    return handle
end

function M.result(handle)
    local g = handles[handle]
    if not g then return nil, "Unknown or released subagent handle" end
    return {done = g.done == true, status = g.status,
        result = g.done and copy(g.output) or nil,
        publication_error = g.publication_error}
end

function M.cancel(handle)
    local g = handles[handle]
    if not g then return false, "Unknown or released subagent handle" end
    cancel_group(g)
    return true
end

-- Transfer failed cleanup to the existing polling reclamation path.
function M.discard(handle)
    local g=handles[handle]
    if not g then return false end
    g.discard=true
    handles[handle]=nil
    cancel_group(g)
    return true
end

function M.release(handle)
    local g = handles[handle]
    if not g then return false, "Unknown or released subagent handle" end
    local ok, err = reclaim(g)
    if ok then handles[handle] = nil end
    return ok, err
end
return M
