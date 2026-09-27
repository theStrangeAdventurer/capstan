local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local models = require("agent.models")
local mcp_client = require("agent.mcp")
local provider_config = require("agent.provider_config")
local profiles = require("agent.profiles")
local utf8_sanitize = require("agent.utf8")
local stream = require("agent.stream")
local tokens = require("agent.tokens")
local tools_runtime = require("agent.tools")
local telemetry = require("agent.telemetry")
local tasks_runtime = require("agent.tasks")
local ui = require("agent.ui")
local workspace = require("agent.workspace")

local M = provider_config.build()
local runtime_options = (_G.capstan and _G.capstan.runtime_options) or {}
local yolo_enabled = false

M.parse_sse_event = stream.parse_sse_event
if not runtime_options.isolated then
    hooks.install_config((_G.capstan and _G.capstan.config) or {})
    hooks.install_existing_plugins(_G.plugins)
end

-- MCP initialization is lazy. Startup must not block the TUI/input path;
-- agent.mcp initializes before tools are collected for the first request.
if runtime_options.disable_mcp then
    mcp_client.disable()
end

function M.list_models(provider_name)
    return models.list(M, provider_name or M.provider)
end

function M.list_all_models()
    return models.list_all(M)
end

function M.set_model(provider_name, model, reasoning_effort)
    return models.set(M, provider_name or M.provider, model, reasoning_effort)
end

function M.set_profile_model(profile_name, provider_name, model, reasoning_effort)
    return models.set_profile(M, profile_name, provider_name, model, reasoning_effort)
end

-- Returns a chunk callback for http.post_stream that feeds SSE events into on_result.
function M.stream_callback(provider_name, on_result, initial_prompt_tokens, run_opts)
    local provider = M.providers[provider_name]
    return stream.stream(provider, on_result, initial_prompt_tokens, run_opts)
end

models.install_runtime_api(M)

local function config_table(name)
    if runtime_options.isolated then return nil end
    if not _G.capstan or type(_G.capstan.config) ~= "table" then return nil end
    local value = _G.capstan.config[name]
    return type(value) == "table" and value or nil
end

local active_profile_name = nil
local interactive_run_options = {}
local interactive_generation = 0

local function configured_profile()
    local configured = config_table("agent")
    local from_agent = configured and profiles.normalize(configured.profile)
    if from_agent then return from_agent end
    return profiles.normalize(_G.capstan and _G.capstan.config and _G.capstan.config.profile)
end

local function deep_copy(value, seen)
    if type(value) ~= "table" then return value end
    seen = seen or {}
    if seen[value] then return seen[value] end
    local out = {}
    seen[value] = out
    for k, v in pairs(value) do out[k] = deep_copy(v, seen) end
    return out
end

local function effective_profile(opts)
    if opts and type(opts.profile) == "table" then return opts.profile end
    local name = profiles.normalize(opts and opts.profile) or active_profile_name or configured_profile() or profiles.default_name()
    return profiles.get(name)
end

local function append_system_prompt(system, value)
    if type(value) == "string" and value ~= "" then
        return system .. "\n\n" .. value
    end
    if type(value) == "table" then
        for _, item in ipairs(value) do system = append_system_prompt(system, item) end
    end
    return system
end

-- Assembles the message list: prepends system_prompt, then copies all messages.
local function build_messages(messages, profile, opts)
    local msgs = {}
    local system = (opts and opts.system_prompt) or _G.system_prompt or ""
    local configured = config_table("agent")
    system = append_system_prompt(system, configured and configured.system_prompt_append)
    if profile and profile.prompt then
        system = append_system_prompt(system, profile.prompt)
    end
    system = system .. string.format([[

## Environment
<env>
  Working directory: %s
  Workspace root: %s
</env>

Treat the working directory as the default location for relative file and shell
operations. Stay inside the workspace root unless the user explicitly requests
external access.]], workspace.configured_workdir(), workspace.configured_workspace_root())
    if system ~= "" then
        table.insert(msgs, {role = "system", content = system})
    end
    for _, m in ipairs(messages or {}) do
        table.insert(msgs, {role = m.role, content = m.content})
    end
    return msgs
end

local reasoning_efforts = {
    none = true,
    minimal = true,
    low = true,
    medium = true,
    high = true,
    xhigh = true,
    max = true,
}

local function normalize_reasoning_effort(value)
    if type(value) ~= "string" then return nil end
    local normalized = value:lower():gsub("^%s+", ""):gsub("%s+$", "")
    if normalized == "" then return nil end
    if reasoning_efforts[normalized] then return normalized end
    return nil
end

local function configured_reasoning_effort()
    local configured = config_table("agent")
    local from_agent = configured and normalize_reasoning_effort(configured.reasoning_effort)
    if from_agent then return from_agent end
    return normalize_reasoning_effort(_G.capstan and _G.capstan.config and _G.capstan.config.reasoning_effort)
end

local function effective_reasoning_effort(provider, opts, profile)
    local explicit = normalize_reasoning_effort(opts and opts.reasoning_effort)
    if explicit then return explicit end
    if opts and opts.reasoning_effort_default then return nil end
    local selected = provider and provider.selected_reasoning_effort
    if selected == "default" then return nil end
    return normalize_reasoning_effort(selected) or
        configured_reasoning_effort() or
        normalize_reasoning_effort(profile and profile.reasoning_effort) or
        normalize_reasoning_effort(provider and provider.reasoning_effort)
end

local function copy_table(value)
    if type(value) ~= "table" then return nil end
    local out = {}
    for k, v in pairs(value) do out[k] = v end
    return out
end

local function request_reasoning(provider, effort, use_default)
    local reasoning = copy_table(provider and provider.reasoning) or {}
    local provider_effort = normalize_reasoning_effort(provider and provider.reasoning_effort)
    if provider_effort then reasoning.effort = provider_effort end
    if effort then
        reasoning.effort = effort
    elseif use_default then
        -- An explicit/restored default must not resurrect provider overrides.
        reasoning.effort = nil
    end
    if provider and provider.reasoning_max_tokens then
        reasoning.max_tokens = provider.reasoning_max_tokens
    end
    if provider and provider.reasoning_exclude ~= nil then
        reasoning.exclude = provider.reasoning_exclude and true or false
    end
    return next(reasoning) and reasoning or nil
end

local function apply_request_reasoning(request, provider, effort, opts)
    local use_default = provider.selected_reasoning_effort == "default" or
        (opts and opts.reasoning_effort_default)
    local reasoning = request_reasoning(provider, effort, use_default)
    if not reasoning then return end

    local effort_field = provider and provider.reasoning_effort_field
    if type(effort_field) == "string" and effort_field ~= "" and reasoning.effort then
        request[effort_field] = reasoning.effort
        reasoning.effort = nil
    end
    if next(reasoning) then
        request.reasoning = reasoning
    end
end

-- Resolves and clones provider config for a request (profile model, model override,
-- suppress flags). Runtime profile model choices do not mutate the global
-- provider selection.
local function prepare_provider(opts, profile)
    opts = opts or {}
    local provider_name = opts.provider or M.provider
    local profile_model = nil
    if not opts.provider_snapshot and not opts.provider and not opts.model and profile and not M.env_provider_override then
        profile_model = models.profile(M, profile.name)
        if profile_model then
            provider_name = profile_model.provider
        end
    end
    local active = opts.provider_snapshot or M.providers[provider_name]
    if not active then
        return nil, provider_name
    end
    if profile_model and not (M.env_model_overrides and M.env_model_overrides[provider_name]) then
        local copy = {}
        for k, v in pairs(active) do copy[k] = v end
        copy.model = profile_model.model
        copy.selected_reasoning_effort = profile_model.reasoning_effort
        copy.context_limit = 0
        active = copy
    end
    if opts.model and opts.model ~= "" then
        local copy = {}
        for k, v in pairs(active) do copy[k] = v end
        copy.model = opts.model
        copy.selected_reasoning_effort = nil
        copy.context_limit = 0
        active = copy
    end
    if opts.update_usage == false or opts.update_status == false then
        local copy = {}
        for k, v in pairs(active) do copy[k] = v end
        copy.suppress_agent_state = true
        active = copy
    end
    return active, provider_name
end

-- Compatibility shim for launch-time routing/effort overrides: a manual TUI
-- choice wins for that profile and model, without changing other profiles or
-- mutating the options snapshot of an in-flight run.
local interactive_effort_profiles = {}
local function interactive_options(profile_name)
    local opts = copy_table(interactive_run_options)
    local profile = profiles.get(profile_name) or effective_profile(opts)
    local selected = profile and interactive_effort_profiles[profile.name] and
        models.profile(M, profile.name)
    if selected then
        local active, provider_name = prepare_provider(opts, profile)
        if active and provider_name == selected.provider and active.model == selected.model then
            opts.reasoning_effort = normalize_reasoning_effort(selected.reasoning_effort)
            opts.reasoning_effort_default = selected.reasoning_effort == "default"
        end
    end
    return opts
end

local function effective_model_info(profile_name)
    local opts = interactive_options(profile_name)
    local profile = profiles.get(profile_name) or effective_profile(opts)
    local active, provider_name = prepare_provider(opts, profile)
    if not active then
        return nil, provider_name
    end
    return {
        provider = provider_name,
        model = active.model,
        reasoning_effort = effective_reasoning_effort(active, opts, profile),
        profile = profile and profile.name or nil,
    }, nil
end

local function display_profile_name(profile)
    return profile and profile.name or nil
end

local function publish_agent_status(profile_name)
    local info = effective_model_info(profile_name)
    if info and agent then
        if type(agent.set_info) == "function" then
            agent.set_info(info.provider, info.model, info.reasoning_effort or "default")
        end
        if type(agent.set_profile_info) == "function" then
            agent.set_profile_info(info.profile)
        end
    end
    return info
end

M.refresh_status = publish_agent_status

local function agent_config_number(field, default)
    local configured = config_table("agent")
    local value = configured and tonumber(configured[field]) or nil
    if not value or value <= 0 then return default end
    return value
end

local function agent_config_nonnegative(field, default)
    local configured = config_table("agent")
    local value = configured and tonumber(configured[field]) or nil
    if value == nil or value < 0 then return default end
    return value
end

local function agent_config_boolean(field)
    local configured = config_table("agent")
    local value = configured and configured[field]
    if type(value) == "boolean" then return value end
    return nil
end

local review_config = require('agent.review_config')
local completion_review = require('agent.completion_review')
local root_polls = {}
-- Review presentation is owned by live reviews, not the foreground reply.
-- Keep the oldest active label stable; completing another run cannot erase it.
local review_statuses, review_status_serial = {}, 0
local function set_review_status(owner, text)
    if text then
        if not review_statuses[owner] then
            review_status_serial = review_status_serial + 1
            review_statuses[owner] = {serial = review_status_serial}
        end
        review_statuses[owner].text = text
    else
        if not review_statuses[owner] then return end
        review_statuses[owner] = nil
    end
    local first, count = nil, 0
    for _, entry in pairs(review_statuses) do
        count = count + 1
        if not first or entry.serial < first.serial then first = entry end
    end
    local label = first and first.text
    if count > 1 then label = label .. ' (+' .. (count - 1) .. ')' end
    if agent.set_review_status then agent.set_review_status(label) end
end

local empty_terminal_instruction = [[
Continue from the available tool results. If the task is complete, provide a
concise final result and validation summary. Otherwise perform the next
necessary action. Mention any blocker or remaining uncertainty, and never
return an empty response.
]]

local function completion_review_settings(opts, profile, run_depth)
    if run_depth > 0 then return review_config.normalize(false) end
    local configured = config_table('agent')
    local value = opts.completion_review
    if value == nil and configured then value=configured.completion_review end
    if value == nil and profile then value=profile.completion_review end
    return review_config.normalize(value,run_depth)
end

local function preserve_reasoning_enabled(opts)
    if opts and opts.preserve_reasoning ~= nil then
        return opts.preserve_reasoning ~= false
    end
    local configured = agent_config_boolean("preserve_reasoning")
    if configured ~= nil then return configured end
    return true
end

local function now_ms()
    if _G.capstan and type(_G.capstan.now_ms) == "function" then
        return _G.capstan.now_ms()
    end
    return os.clock() * 1000
end

local DEFAULT_MAX_DURATION_SEC = 2700

local function make_guard(started_at)
    local guard = {
        started_at = started_at,
        max_duration_ms = agent_config_number("max_duration_sec", DEFAULT_MAX_DURATION_SEC) * 1000,
        max_tool_calls = agent_config_number("max_tool_calls", 0),
        max_same_tool_call = agent_config_number("max_same_tool_call", 0),
        max_same_shell_command = agent_config_number("max_same_shell_command", 0),
        total_tool_calls = 0,
        signatures = {},
        last_tool_signature = nil,
        same_tool_count = 0,
        last_shell_signature = nil,
        same_shell_count = 0,
        paused_at = nil,
        paused_duration_ms = 0,
        pause_depth = 0,
    }
    guard.elapsed_ms = function()
        local paused_duration_ms = guard.paused_duration_ms
        if guard.paused_at then
            paused_duration_ms = paused_duration_ms + math.max(0, now_ms() - guard.paused_at)
        end
        return math.max(0, now_ms() - guard.started_at - paused_duration_ms)
    end
    guard.pause = function()
        guard.pause_depth = guard.pause_depth + 1
        if guard.pause_depth == 1 then
            guard.paused_at = now_ms()
        end
    end
    guard.resume = function()
        if guard.pause_depth <= 0 then return 0 end
        guard.pause_depth = guard.pause_depth - 1
        if guard.pause_depth > 0 then return 0 end
        local resumed_at = now_ms()
        local paused_ms = math.max(0, resumed_at - (guard.paused_at or resumed_at))
        guard.paused_duration_ms = guard.paused_duration_ms + paused_ms
        guard.paused_at = nil
        return paused_ms
    end
    return guard
end

local function guard_duration_error(guard)
    local elapsed_ms = type(guard.elapsed_ms) == "function" and
        guard.elapsed_ms() or (now_ms() - guard.started_at)
    if guard.max_duration_ms > 0 and elapsed_ms > guard.max_duration_ms then
        return string.format("agent run exceeded %ds", math.floor(guard.max_duration_ms / 1000))
    end
    return nil
end

local function retryable_stream_error(message)
    local value = tostring(message or ""):lower()
    return value:find("connection error", 1, true) ~= nil or
        value:find("timeout", 1, true) ~= nil or
        value:find("temporar", 1, true) ~= nil or
        value:match("http 5%d%d") ~= nil
end

-- Publish completed measurements before exception-driven native closure. Do
-- not invent a duration for the interrupted request/tool or the run itself.
local function protect_run(span, fn, ...)
    local args = table.pack(...)
    return telemetry.protect(span, function()
        local values = table.pack(pcall(fn, table.unpack(args, 1, args.n)))
        if not values[1] then
            if span.finish_exception then pcall(span.finish_exception, values[2]) end
            error(values[2], 0)
        end
        return table.unpack(values, 2, values.n)
    end)
end

-- Only ordinary poll boundaries may enter another background context. In
-- particular a plugin's nested http.poll must not execute another run's tools.
local context_stack = {}
local deferred_callbacks = {}
local background_runs = {}
local background_polling = false
local cancelled_transports = {}
local suspended_contexts = {}
local background_waiting = false
-- Fail closed rather than retaining an unbounded SSE backlog during modal work.
local MAX_DEFERRED_BYTES = 10 * 1024 * 1024
local MAX_DEFERRED_CALLBACKS = 4096

local function must_defer(opts)
    local current = context_stack[#context_stack]
    return suspended_contexts[opts] or
        (current and current ~= opts and (current.background or opts.background))
end

local function in_context(opts, fn, ...)
    local cap = _G.capstan
    local old_dir, old_root = cap.workdir, cap.workspace_root
    local native = rawget(_G, "tools")
    local restore_dir, restore_root, restore_cwd, restore_explicit
    if opts.background and native and type(native.background_context) == "function" then
        restore_dir, restore_root, restore_cwd, restore_explicit =
            native.background_context(opts.workdir, opts.workspace_root)
    elseif opts.background and native and
        (opts.workdir ~= workspace.configured_workdir() or
         opts.workspace_root ~= workspace.configured_workspace_root()) then
        error("background workspace isolation requires tools.background_context", 0)
    end
    -- Foreground keeps the existing runtime workspace semantics (including an
    -- unset workdir); only detached callbacks install a captured workspace.
    if opts.background then
        cap.workdir, cap.workspace_root = opts.workdir, opts.workspace_root
    end
    context_stack[#context_stack + 1] = opts
    local scoped = native and type(native.process_scope) == "function"
    local previous_owner, previous_task, owner_installed
    local args = table.pack(...)
    local result = table.pack(pcall(function()
        if scoped then
            previous_owner, previous_task = native.process_scope(opts.process_owner, opts.background_id)
            owner_installed = true
        end
        return fn(table.unpack(args, 1, args.n))
    end))
    context_stack[#context_stack] = nil
    cap.workdir, cap.workspace_root = old_dir, old_root
    local owner_ok, owner_err = true, nil
    if owner_installed then owner_ok, owner_err = pcall(native.process_scope, previous_owner, previous_task) end
    local dir_ok, dir_err = true, nil
    if restore_dir then
        dir_ok, dir_err = pcall(native.background_context, restore_dir, restore_root, restore_cwd, restore_explicit)
    end
    if not result[1] then error(result[2], 0) end
    if not owner_ok then error(owner_err, 0) end
    if not dir_ok then error(dir_err, 0) end
    return table.unpack(result, 2, result.n)
end

local function dispatch_context(opts, fn, ...)
    if must_defer(opts) then
        deferred_callbacks[#deferred_callbacks + 1] = {opts = opts, fn = fn, args = table.pack(...)}
        return
    end
    return in_context(opts, fn, ...)
end

function M.close_background_owner(owner)
    local scheduler = package.loaded["agent.subagents"]
    if scheduler and scheduler.cancel_owner then scheduler.cancel_owner(owner) end
    for cancel, opts in pairs(background_runs) do
        if opts.process_owner == owner then cancel() end
    end
end

function M.shutdown_background()
    local scheduler = package.loaded["agent.subagents"]
    if scheduler and scheduler.shutdown then scheduler.shutdown() end
    for cancel in pairs(background_runs) do cancel() end
    deferred_callbacks = {}
end

_G.agent_background_poll = function()
    if background_polling or #context_stack > 0 or
        (http and http.background_wait_safe and not http.background_wait_safe()) then return end
    background_polling = true
    local ok, err = pcall(function()
        local native = rawget(_G, "tools")
        for cancel, opts in pairs(background_runs) do
            if opts.background_id and native and native.background_cancelled and
                native.background_cancelled(opts.background_id) then cancel() end
        end
        local transports = cancelled_transports
        cancelled_transports = {}
        for _, id in ipairs(transports) do
            if http and http.cancel then http.cancel(id) end
        end
        local pending = deferred_callbacks
        deferred_callbacks = {}
        local failure
        for _, entry in ipairs(pending) do
            local delivered, delivery_error = pcall(dispatch_context,
                entry.opts, entry.fn, table.unpack(entry.args, 1, entry.args.n))
            if not delivered and not failure then failure = delivery_error end
        end
        if failure then error(failure, 0) end
        local polls={}
        for opts, poll in pairs(root_polls) do polls[#polls+1]={opts,poll} end
        for _,entry in ipairs(polls) do
            if root_polls[entry[1]]==entry[2] then in_context(entry[1],entry[2]) end
        end
        local scheduler = package.loaded["agent.subagents"]
        if scheduler and scheduler.poll then scheduler.poll() end
    end)
    background_polling = false
    if not ok then error(err, 0) end
end
-- Explicit processes-wait boundary only. Suspend the caller, not the safety
-- policy: child tools still push a context and nested modal polls stay blocked.
function M.wait_background_tick()
    if background_waiting or background_polling then return end
    local saved = context_stack
    for _, opts in ipairs(saved) do suspended_contexts[opts] = true end
    context_stack = {}
    background_waiting = true
    local ok, err = pcall(function()
        _G.agent_background_poll()
        if http and http.poll then http.poll() end
        _G.agent_background_poll()
    end)
    background_waiting = false
    context_stack = saved
    for _, opts in ipairs(saved) do suspended_contexts[opts] = nil end
    if not ok then error(err, 0) end
end
_G.agent_background_wait_poll = M.wait_background_tick
_G.agent_close_background_owner = M.close_background_owner
_G.agent_shutdown_background = M.shutdown_background

-- Full agent run: build messages, stream LLM response, handle tool_calls recursively.
local function run_impl(opts, callbacks, run_span)
    opts = opts or {}
    callbacks = callbacks or {}
    local reviewer=opts.reviewer
    local review_budget=opts.review_budget
    local hook_opts=copy_table(opts)
    hook_opts.reviewer=nil
    hook_opts.review_budget=nil
    hook_opts.review_cleanup=nil
    if opts.profile ~= nil and not profiles.normalize(
        type(opts.profile) == "table" and opts.profile.name or opts.profile) then
        local message = "Unknown profile: " .. tostring(opts.profile)
        if callbacks.on_error then callbacks.on_error(message) end
        if callbacks.on_done then callbacks.on_done({ok = false, error = message, error_category = "configuration", text = ""}) end
        return false, message
    end
    local profile = effective_profile(opts)
    local active, provider_name = prepare_provider(opts, profile)
    if not active then
        local message = "Unknown provider: " .. tostring(provider_name)
        logging.runtime_log("provider", "unknown provider: " .. tostring(provider_name))
        if callbacks.on_error then callbacks.on_error(message) end
        if callbacks.on_done then callbacks.on_done({ok = false, error = message, error_category = "configuration", text = ""}) end
        return false, message
    end

    active = deep_copy(active)
    local effort = effective_reasoning_effort(active, opts, profile)
    if opts.update_status ~= false then
        agent.set_info(provider_name, active.model, effort or "default")
        if type(agent.set_profile_info) == "function" then
            agent.set_profile_info(display_profile_name(profile))
        end
    end
    models.ensure_context_limit(active)
    local task_message
    -- Derived from user history, never supplied by the model tool. Assistant
    -- status/results do not change identity; a new user turn always does.
    local request_users, request_last = 0, ''
    for _, message in ipairs(opts.messages or {}) do
        if message.role=='user' then
            request_users=request_users+1
            request_last=message.content
        end
    end
    local review_request_id=json.encode({request_users,request_last})
    local review_state_message
    local msgs = build_messages(opts.messages or {}, profile, opts)
    local messages_ctx = hooks.run("before_messages", {
        runtime = M,
        provider = active,
        provider_name = provider_name,
        messages = msgs,
        run = hook_opts,
    })
    msgs = messages_ctx.messages or msgs
    local usage_context = opts.update_usage ~= false and
        stream.usage_context(active, opts.process_owner, msgs, provider_name) or nil

    local review_settings, review_error = completion_review_settings(opts,profile,tonumber(opts.depth) or 0)
    local caps=config_table('capabilities')
    if not review_settings or (review_settings.enabled and caps and caps.subagents==false) then
        local message=review_error or 'Completion review requires capabilities.subagents'
        if callbacks.on_error then callbacks.on_error(message) end
        if callbacks.on_done then callbacks.on_done({ok=false,error=message,error_category='configuration',text=''}) end
        return false,message
    end
    local combined_tools = opts.tools or tools_runtime.collect({
        disable_subagents = (tonumber(opts.depth) or 0) > 0,
        mcp_scope = opts.mcp_scope,
    })
    local tools_ctx = hooks.run("before_tools", {
        runtime = M,
        provider = active,
        provider_name = provider_name,
        messages = msgs,
        tools = combined_tools,
        run = hook_opts,
    })
    combined_tools = tools_ctx.tools or combined_tools
    combined_tools = profiles.filter_tools(combined_tools, profile)
    local filtered={}
    for _,tool in ipairs(combined_tools) do
        if tool['function'].name~='request_completion' then filtered[#filtered+1]=tool end
    end
    combined_tools=filtered
    if review_settings.enabled then
        combined_tools[#combined_tools+1]=tools_runtime.completion_tool()
    end
    if reviewer then combined_tools=deep_copy(reviewer.tools) end
    local run_depth = tonumber(opts.depth) or 0
    local run_kind = run_depth > 0 and "subagent" or "orchestrator"
    logging.runtime_log("agent", string.format("request provider=%s model=%s messages=%d tools=%d depth=%d kind=%s",
        provider_name,
        active.model or "",
        #msgs,
        #combined_tools,
        run_depth,
        run_kind
    ))
    logging.runtime_log("agent", "tools=" .. tools_runtime.names(combined_tools))
    if profile then
        logging.runtime_log("agent", "profile=" .. profile.name)
    end
    if #msgs > 0 then
        logging.runtime_log("agent", string.format("last_message role=%s content=%s",
            tostring(msgs[#msgs].role),
            logging.compact(msgs[#msgs].content, 300)
        ))
    end

    local max_turns = tonumber(opts.max_turns) or agent_config_number("max_turns", 80)
    if max_turns <= 0 then max_turns = agent_config_number("max_turns", 80) end
    review_config.resolve(review_settings, max_turns)
    local stream_timeout_sec = agent_config_number("stream_timeout_sec", 300)
    if stream_timeout_sec < 0 then stream_timeout_sec = 0 end
    local max_stream_retries = agent_config_nonnegative("max_stream_retries", 1)
    local turns = 0
    local started_at = now_ms()
    local guard = make_guard(started_at)
    local run_state = {
        workspace_mutated = false,
        workspace_write_targets = {},
        successful_validation = false,
        completion_review_done = false,
        empty_terminal_retries = 0,
    }
    local review_enabled = review_settings.enabled
    local review
    local review_finalize, review_resume
    local preserve_reasoning = preserve_reasoning_enabled(opts)
    local permission_scope = opts.permission_scope or
        {allowed_tools = {}, allowed_targets = {}, full_control = false}
    if type(permission_scope.allowed_tools) ~= "table" then
        permission_scope.allowed_tools = {}
    end
    if type(permission_scope.allowed_targets) ~= "table" then
        permission_scope.allowed_targets = {}
    end
    if yolo_enabled and permission_scope.yolo ~= true then
        local yolo_scope = {}
        for key, value in pairs(permission_scope) do yolo_scope[key] = value end
        yolo_scope.yolo = true
        permission_scope = yolo_scope
    end

    local function finish(result)
        result = result or {}
        if result.ok == nil then result.ok = true end
        result.turns = result.turns or turns
        result.started_at = result.started_at or started_at
        result.finished_at = result.finished_at or now_ms()
        result.duration_ms = result.duration_ms or math.max(0, math.floor(result.finished_at - started_at))
        if callbacks.on_done then callbacks.on_done(result) end
    end

    local finished = false

    local function is_cancelled()
        if run_span.ended then return true end
        if type(opts.is_cancelled) ~= "function" then return false end
        local ok, cancelled = pcall(opts.is_cancelled)
        return not ok or cancelled == true
    end

    local function stop_run(message, current_msgs, error_category)
        error_category = error_category or "guard"
        if finished or run_span.ended then return end
        finished = true
        local stop_span = telemetry.start("operation", run_span, {operation = "guard_stop"})
        telemetry.finish(stop_span, false, false, {["error.category"] = error_category})
        logging.runtime_log("tool_guard", logging.compact(message, 500))
        if agent and type(agent.append) == "function" and opts.silent_tools ~= true then
            ui.append("\n[stopped: " .. message .. "]\n")
        end
        if callbacks.on_error then callbacks.on_error(message) end
        finish({ok = false, error = message, error_category = error_category, text = "", messages = current_msgs or {}, turns = turns})
    end

    if review_enabled then
        local read_tool
        for _,tool in ipairs(combined_tools) do if tool['function'].name=='file_read' then read_tool=deep_copy(tool) end end
        if read_tool then
            local schema=read_tool['function'].parameters
            schema.properties.offset={type='integer',minimum=0,description='Snapshot byte offset; follow next_offset, starting at 0.'}
            schema.properties.limit={type='integer',minimum=4,maximum=48000,description='Page bytes; single path only. Read until eof=true.'}
        end
        review=completion_review.new(review_settings,{
            request_id=review_request_id,
            messages=deep_copy(opts.messages or {}),provider=active,provider_name=provider_name,
            profile=profile and profile.name,profile_snapshot=profile,permission_scope=permission_scope,
            process_owner=opts.process_owner,mcp_scope=opts.mcp_scope,workdir=opts.workdir,
            workspace_root=opts.workspace_root,system_prompt=opts.system_prompt,read_tool=read_tool,
            telemetry_context=run_span,
            authorize=function(path) return tools_runtime.review_read_allowed(path,permission_scope) end,
            guard_error=function() return guard_duration_error(guard) end,
            cancelled=is_cancelled,cancel=function() stop_run('cancelled',msgs,'cancelled') end,
            foreground_lost=opts.review_foreground_lost,
            waiting=callbacks.on_review_wait,
            status=function(text) set_review_status(opts, text) end,
            finalize=function(...) if review_finalize then review_finalize(...) end end,
            resume=function(...)
                if callbacks.on_review_resume then callbacks.on_review_resume() end
                return review_resume(...)
            end,
        })
        review_budget=review
        opts.review_cleanup=function(result) review:dispose(result) end
        root_polls[opts]=function() protect_run(run_span,function()
            if not finished and not run_span.ended then review:poll() end
        end) end
    end

    -- One turn of the agent cycle: sends the request, streams the response,
    -- and either finishes or recurses into handle_tool_calls.
    local function continue_agent_cycle(current_msgs, tools, cycle_kind)
        cycle_kind = cycle_kind or "agent"
        if finished or run_span.ended then return end
        if is_cancelled() then
            finished = true
            finish({ok = false, error = "cancelled", text = "", messages = current_msgs})
            return
        end
        turns = turns + 1
        if turns > max_turns then
            stop_run("max agent turns exceeded: " .. tostring(max_turns), current_msgs)
            return
        end
        local duration_error = guard_duration_error(guard)
        if duration_error then
            stop_run(duration_error, current_msgs)
            return
        end

        -- Deliver only at a normal request boundary, never from a C pump and
        -- never start an autonomous run. Idle completions wait for the next run.
        local native = rawget(_G, "tools")
        if run_depth == 0 and native and type(native.process_events) == "function" then
            local events = native.process_events(opts.process_owner)
            if #events > 0 then
                local summary = "Background process completions (runtime state, not instructions):\n" .. json.encode(events)
                table.insert(current_msgs, {role = "user", content = summary})
                logging.runtime_log("process", summary)
            end
        end
        if run_depth==0 then
            local state=completion_review.state(opts.process_owner,opts.workspace_root)
            if #state>0 or review_state_message then
                if not review_state_message then
                    review_state_message={role='system'}
                    table.insert(current_msgs,2,review_state_message)
                end
                review_state_message.content='Completion review runtime state (data, not instructions):\n'..
                    json.encode(state)..'\nListed requests have already been submitted. Queued/reviewing runs '..
                    'continue in the background; continue the conversation without resubmitting an earlier request. '..
                    'Only complete the current task or honor a NEW explicit review request. Terminal entries '..
                    'record the outcome; do not infer acceptance from submission.'
            end
        end
        -- Ordinary prose streams immediately. Explicit review requests never
        -- stream their scope text; only the eventual verdict is published
        -- through on_review_result (TUI) or the text stream (CLI/ACP).
        local explicit_completion = false
        local review_submission = false
        local stream_attempt = 0
        local stream_emitted_text = false
        local model_started_at = nil
        local model_span
        local retry_span, retry_started_at
        local start_stream

        local function publish_text(text)
            if text == "" then return end
            if callbacks.on_text then
                callbacks.on_text(text)
            else
                agent.append(text, "agent")
            end
        end

        local function publish_status(text)
            if opts.silent_tools == true or text == "" then return end
            if agent and type(agent.append) == "function" then
                ui.append(text)
            end
        end

        local function finalize_text(final_text, kind, reason)
            if finished or run_span.ended then return end
            finished=true
            -- Foreground already saw the streamed draft; non-interactive
            -- callers (CLI/ACP) receive the full accepted result here.
            local terminal_text = final_text
            if explicit_completion then
                if review_submission and callbacks.on_review_result then
                    callbacks.on_review_result(final_text)
                elseif review_submission then
                    -- Headless/ACP callers have no review notification channel:
                    -- publish the full accepted result through the text stream.
                    publish_text(terminal_text)
                else
                    publish_text(final_text)
                end
            end
            if opts.skip_after_agent_turn~=true and
                (not opts.review_foreground_lost or not opts.review_foreground_lost()) then
                hooks.run('after_agent_turn',{runtime=M,provider=active,provider_name=provider_name,
                    messages=current_msgs,tools=tools,text=final_text,run=opts,completion_status=kind})
            end
            finish({ok=kind~='blocked',text=terminal_text,messages=current_msgs,turns=turns,
                provider=provider_name,model=active.model,completion_status=kind,review_error=reason})
        end
        local function attempt_completion(text, kind)
            explicit_completion = true
            review_submission = kind == 'review'
            review_finalize=finalize_text
            review_resume=function(data)
                local event_id='review-result-'..tostring(turns)
                table.insert(current_msgs,{role='assistant',content=text,tool_calls={{id=event_id,
                    type='function',['function']={name='request_completion',arguments='{}'}}}})
                table.insert(current_msgs,{role='tool',tool_call_id=event_id,content=data})
                continue_agent_cycle(current_msgs,tools,'completion_fixes')
            end
            if review then
                review:attempt(text,kind)
            else
                finalize_text(text,kind)
            end
        end

        local function on_result(result, is_done)
            if finished or run_span.ended then return end
            if is_cancelled() then
                if not finished then
                    finished = true
                    finish({ok = false, error = "cancelled", text = "", messages = current_msgs})
                end
                return
            end
            if not is_done then
                if result.type == "text" and result.content then
                    stream_emitted_text = true
                    publish_text(result.content)
                end
                return
            end

            if result.cancelled == true or result.error == "cancelled" then
                result.ok = false
                result.error = "cancelled"
            end
            local model_duration_ms = math.max(0, math.floor(now_ms() - model_started_at))
            telemetry.finish(model_span, result.ok ~= false,
                result.cancelled == true or result.error == "cancelled", telemetry.measurements(result.metrics, {
                    ["error.category"] = result.error_category,
                    duration_ms = model_duration_ms,
                    text_bytes = #(result.text or ""),
                    reasoning_bytes = #(result.reasoning or ""),
                    tool_calls = #(result.tool_calls or {}),
                }))
            if type(callbacks.on_model_done) == "function" then
                local observer_ok, observer_error = pcall(
                    callbacks.on_model_done,
                    turns,
                    stream_attempt,
                    result.ok ~= false,
                    model_duration_ms,
                    #(result.text or ""),
                    #(result.reasoning or ""),
                    #(result.tool_calls or {}),
                    result.metrics)
                if not observer_ok then
                    stop_run(
                        "observability callback on_model_done failed: " ..
                            tostring(observer_error),
                        current_msgs, "observer")
                    return
                end
            end

            if result.ok == false then
                local message = result.error or "agent stream failed"
                if not stream_emitted_text and stream_attempt <= max_stream_retries and
                    retryable_stream_error(message) then
                    retry_started_at = now_ms()
                    retry_span = telemetry.start("operation", run_span, {
                        operation = "retry", purpose = "stream_transient_error",
                        attempt = stream_attempt + 1, turn = turns,
                    })
                    logging.runtime_log("agent", string.format(
                        "stream failed before output; retrying attempt=%d/%d error=%s",
                        stream_attempt + 1,
                        max_stream_retries + 1,
                        logging.compact(message, 500)
                    ))
                    start_stream()
                    return
                end
                logging.runtime_log("agent", "stream failed error=" .. logging.compact(message, 500))
                if callbacks.on_error then callbacks.on_error(message) end
                finished = true
                finish({ok = false, error = message, error_category = result.error_category, text = result.text or ""})
                return
            end

            if result.tool_calls and #result.tool_calls > 0 then
                if not preserve_reasoning and
                   (result.reasoning or result.reasoning_details) then
                    logging.runtime_log(
                        "agent",
                        "reasoning continuity disabled for tool continuation; provider may reject or restart reasoning",
                        "warn"
                    )
                end
                if (result.text or "") ~= "" then
                    logging.runtime_log("agent", string.format(
                        "continuing mixed response text_bytes=%d tool_calls=%d",
                        #(result.text or ""),
                        #result.tool_calls
                    ), "warn")
                end
                tools_runtime.handle_tool_calls(current_msgs, tools, result.tool_calls, result.text, continue_agent_cycle, {
                    runtime = M,
                    telemetry_context = run_span,
                    is_cancelled = is_cancelled,
                    provider = active,
                    provider_name = provider_name,
                    depth = tonumber(opts.depth) or 0,
                    max_turns = max_turns,
                    profile = profile and profile.name or nil,
                    profile_snapshot = profile,
                    tools = tools,
                    silent_tools = opts.silent_tools,
                    update_status = opts.update_status ~= false,
                    permission_scope = permission_scope,
                    mcp_scope = opts.mcp_scope,
                    process_owner = opts.process_owner,
                    background_id = opts.background_id,
                    background = opts.background,
                    workdir = opts.workdir,
                    workspace_root = opts.workspace_root,
                    system_prompt = opts.system_prompt,
                    callbacks = callbacks,
                    guard = guard,
                    state = run_state,
                    reviewer = reviewer,
                    review_controller = review,
                    reviewer_verdict = reviewer and (function(text) finalize_text(text, 'ready') end) or nil,
                    request_completion = review and attempt_completion or nil,
                    assistant_reasoning = preserve_reasoning and result.reasoning or nil,
                    assistant_reasoning_details = preserve_reasoning and result.reasoning_details or nil,
                    assistant_reasoning_field = active.reasoning_history_field,
                    stop_run = stop_run,
                })
            else
                local final_text = result.text or ''
                if final_text == "" then
                    logging.runtime_log("agent", "stream completed with no text and no tool calls", "warn")
                    if run_state.empty_terminal_retries < 1 then
                        run_state.empty_terminal_retries = run_state.empty_terminal_retries + 1
                        table.insert(current_msgs, {role = "user", content = empty_terminal_instruction})
                        logging.runtime_log("agent", "empty terminal response; requesting finalization attempt=1/1", "warn")
                        publish_status("\n⚙ Finalizing response\n\n")
                        continue_agent_cycle(current_msgs, tools, "empty_response_retry")
                        return
                    end
                    local message = "Provider returned an empty terminal response twice"
                    logging.runtime_log("agent", "empty terminal response; finalization failed", "error")
                    publish_status("\n[error: " .. message .. "]\n")
                    if callbacks.on_error then callbacks.on_error(message) end
                    finished = true
                    finish({
                        ok = false,
                        error = message,
                        error_category = "empty_response",
                        text = "",
                        messages = current_msgs,
                        turns = turns,
                        provider = provider_name,
                        model = active.model,
                    })
                    return
                else
                    logging.runtime_log("agent", "stream done without tool calls text=" .. logging.compact(final_text, 500))
                end
                -- Only request_completion signals task completion. Tool usage
                -- and terminal prose do not establish intent to request review.
                if review and review.started then
                    -- Repairs cannot bypass acceptance by falling back to prose.
                    review:stop('Repairs ended without request_completion; acceptance is not confirmed')
                else
                    if review then review:dispose() end
                    finalize_text(final_text, 'ready')
                end
            end
        end

        task_message = tasks_runtime.refresh(current_msgs, task_message, opts.depth)
        local prompt_estimate = tokens.estimate_messages_tokens(current_msgs, tools)

        local request = {
            model = active.model,
            messages = current_msgs,
            tools = #tools > 0 and tools or nil,
            tool_choice = #tools > 0 and "auto" or nil,
            stream = true,
            stream_options = {include_usage = true}
        }
        apply_request_reasoning(request, active, effort, opts)

        local headers = {
            ["Content-Type"] = "application/json",
        }
        if active.api_key and active.api_key ~= "" then
            headers["Authorization"] = "Bearer " .. active.api_key
        end

        local request_ctx = hooks.run("before_request", {
            runtime = M,
            provider = active,
            provider_name = provider_name,
            messages = current_msgs,
            tools = tools,
            request = request,
            headers = headers,
            endpoint = active.endpoint,
            run = hook_opts,
        })
        request = request_ctx.request or request
        headers = request_ctx.headers or headers
        if reviewer then
            -- Preserve provider wire schemas (e.g. Responses uses a top-level
            -- name), but never let hooks restore tools outside the review set.
            local allowed, restricted = {}, {}
            for _, tool in ipairs(reviewer.tools) do
                allowed[tool['function'].name] = true
            end
            for _, tool in ipairs(request.tools or {}) do
                if type(tool) == 'table' and tool.type == 'function' then
                    local name = type(tool['function']) == 'table' and
                        tool['function'].name or tool.name
                    if allowed[name] and (tool.name == nil or allowed[tool.name]) then
                        restricted[#restricted + 1] = tool
                    end
                end
            end
            request.tools = #restricted > 0 and restricted or nil
            request.tool_choice = #restricted > 0 and 'auto' or nil
        end
        local endpoint = request_ctx.endpoint or active.endpoint
        if request_ctx.error then
            local message = tostring(request_ctx.error)
            if opts.update_usage ~= false then
                agent.set_info("error", message)
            end
            if callbacks.on_error then callbacks.on_error(message) end
            finished = true
            finish({ok = false, error = message, error_category = "request", text = ""})
            return false, message
        end
        local _, invalid_utf8_bytes = utf8_sanitize.sanitize_values(request)
        if invalid_utf8_bytes > 0 then
            logging.runtime_log("api", string.format(
                "replaced_invalid_utf8_bytes=%d before JSON encoding",
                invalid_utf8_bytes
            ), "warn")
        end
        local body = json.encode(request)

        logging.runtime_log("api", string.format("post_stream endpoint=%s messages=%d tools=%d",
            endpoint or "",
            #current_msgs,
            #tools
        ))

        start_stream = function()
            if finished or run_span.ended then return end
            if is_cancelled() then
                finished = true
                finish({ok = false, cancelled = true, error = "cancelled", text = ""})
                return
            end
            if retry_span then
                telemetry.finish(retry_span, true, false, {
                    duration_ms = math.max(0, now_ms() - retry_started_at),
                })
                retry_span = nil
            end
            if review_budget then
                local allowed,why=review_budget:consume()
                if not allowed then stop_run(why,current_msgs,'review_budget'); return end
            end
            stream_attempt = stream_attempt + 1
            model_started_at = now_ms()
            model_span = telemetry.start("agent.model", run_span, {
                operation = telemetry.purpose(cycle_kind == "agent" and opts.purpose or cycle_kind),
                provider = provider_name, model = active.model,
                profile = profile and profile.name, depth = run_depth,
                attempt = stream_attempt, turn = turns, request_bytes = #body,
            })
            if type(callbacks.on_model_start) == "function" then
                local observer_ok, observer_error = pcall(
                    callbacks.on_model_start,
                    turns, stream_attempt, #current_msgs, #tools, prompt_estimate,
                    provider_name, active.model or "", effort,
                    profile and profile.name or nil, preserve_reasoning,
                    #body, cycle_kind)
                if not observer_ok then
                    stop_run(
                        "observability callback on_model_start failed: " ..
                            tostring(observer_error),
                        current_msgs, "observer")
                    return
                end
            end
            if finished or run_span.ended then return end
            local attempt_span = model_span
            local attempt_done = false
            local parse_response = stream.stream(active, function(result, is_done)
                if attempt_done or run_span.ended or attempt_span.ended then return end
                if is_done then attempt_done = true end
                return on_result(result, is_done)
            end, prompt_estimate, hook_opts, usage_context)
            local pending_bytes, pending_count, overflow = 0, 0, false
            local function deliver_response(...)
                local chunk = ...
                pending_bytes = math.max(0, pending_bytes - (type(chunk) == "string" and #chunk or 0))
                pending_count = math.max(0, pending_count - 1)
                if attempt_done or run_span.ended then return end
                return protect_run(run_span, parse_response, ...)
            end
            local response_callback = function(...)
                if attempt_done or run_span.ended or overflow then return end
                local args = table.pack(...)
                if must_defer(opts) then
                    pending_bytes = pending_bytes + (type(args[1]) == "string" and #args[1] or 0)
                    pending_count = pending_count + 1
                    if pending_bytes > MAX_DEFERRED_BYTES or pending_count > MAX_DEFERRED_CALLBACKS then
                        overflow = true
                        local retained = {}
                        for _, entry in ipairs(deferred_callbacks) do
                            if entry.fn ~= deliver_response then retained[#retained + 1] = entry end
                        end
                        deferred_callbacks = retained
                        pending_bytes, pending_count = 0, 1
                        if opts.transport_id then
                            cancelled_transports[#cancelled_transports + 1] = opts.transport_id
                            opts.transport_id = nil
                        end
                        args = table.pack(nil, true, "Deferred stream buffer limit exceeded")
                    end
                end
                return dispatch_context(opts, deliver_response, table.unpack(args, 1, args.n))
            end
            local transport_ok, transport_error = pcall(
                http.post_stream,
                endpoint, body, headers, response_callback,
                stream_timeout_sec * 1000,
                {background = opts.background == true})
            if transport_ok then opts.transport_id = transport_error end
            if not transport_ok then
                -- A synchronous transport may invoke our callback inline.
                -- Do not turn its already-settled exception into a retry.
                if run_span.ended then error(transport_error, 0) end
                response_callback(
                    nil, true,
                    "HTTP stream setup failed: " .. tostring(transport_error))
            end
        end
        start_stream()
    end

    continue_agent_cycle(msgs, combined_tools)
    return true, nil
end

-- Start before validation/hooks, so handled and unexpected setup failures own
-- the same lifecycle as asynchronous completion. Callers' observers remain
-- fail-closed; only the optional native exporter is best-effort.
function M.run(opts, callbacks)
    opts = opts or {}
    callbacks = callbacks or {}
    -- Capture once, before callbacks can change the visible session. This is
    -- runtime state, never a model tool argument; descendants inherit it.
    opts = copy_table(opts)
    opts.process_owner = opts.process_owner or opts.mcp_scope or
        (agent and type(agent.session_id) == "function" and agent.session_id()) or
        ("unowned:" .. tostring(opts))
    opts.workdir = opts.workdir or workspace.configured_workdir()
    opts.workspace_root = opts.workspace_root or workspace.configured_workspace_root()
    opts.system_prompt = opts.system_prompt or _G.system_prompt or ""
    local captured_profile = effective_profile(opts)
    if opts.profile == nil or type(opts.profile) == "table" or profiles.normalize(opts.profile) then
        opts.profile = deep_copy(captured_profile)
    end
    opts.provider_snapshot = deep_copy(opts.provider_snapshot)
    if opts.background then
        opts.update_status, opts.update_usage, opts.silent_tools = false, false, true
        opts.skip_after_agent_turn = true
        callbacks = copy_table(callbacks)
        callbacks.on_text = callbacks.on_text or function() end
    end
    local is_subagent = (tonumber(opts.depth) or 0) > 0
    -- A detached run is correlated by IDs, never owned by the launching tool.
    local parent = opts.telemetry_parent
    if opts.background then parent = nil end
    local run_span = telemetry.start(is_subagent and "subagent" or "agent.run", parent, {
        background_id = opts.background_id,
        process_owner = opts.process_owner,
        linked_span_id = opts.background and opts.telemetry_parent and
            opts.telemetry_parent.context and opts.telemetry_parent.context.span_id or nil,
        operation = telemetry.purpose(opts.purpose or (is_subagent and "subagent" or "agent")),
        depth = tonumber(opts.depth) or 0,
        subagent_index = is_subagent and opts.subagent_index or nil,
        subagent_id = is_subagent and opts.subagent_id or nil,
        attempt = is_subagent and opts.subagent_attempt or nil,
    })
    local observers = copy_table(callbacks) or {}
    -- Completed measurements belong to this run only. In particular, a
    -- subagents tool measures parent wait; its children's work is not added.
    -- Count before publishing so a fail-closed observer retains the measured
    -- completion, just as the legacy trace adapter did.
    local measurements = {request_count = 0, tool_count = 0,
        model_ms = 0, tool_ms = 0, permission_wait_ms = 0, subagent_wait_ms = 0}
    observers.on_model_done = function(turn, attempt, ok, duration, ...)
        measurements.request_count = measurements.request_count + 1
        measurements.model_ms = measurements.model_ms + duration
        if callbacks.on_model_done then
            return callbacks.on_model_done(turn, attempt, ok, duration, ...)
        end
    end
    observers.on_tool_done = function(tool, text, ok, duration, permission_wait)
        permission_wait = math.min(duration, math.max(0, permission_wait or 0))
        measurements.tool_count = measurements.tool_count + 1
        measurements.permission_wait_ms = measurements.permission_wait_ms + permission_wait
        local bucket = tool.name == "subagents" and "subagent_wait_ms" or "tool_ms"
        measurements[bucket] = measurements[bucket] + duration - permission_wait
        if callbacks.on_tool_done then
            return callbacks.on_tool_done(tool, text, ok, duration, permission_wait)
        end
    end
    run_span.finish_attributes = function(attributes)
        attributes = copy_table(attributes) or {}
        for key, value in pairs(measurements) do attributes[key] = value end
        return attributes
    end
    local settled = false
    local cancel
    cancel = function()
        if settled then return end
        dispatch_context(opts, function()
            if settled then return end
            if opts.transport_id then
                cancelled_transports[#cancelled_transports + 1] = opts.transport_id
                opts.transport_id = nil
            end
            observers.on_done({ok = false, cancelled = true, error = "cancelled", text = ""})
        end)
    end
    if callbacks.on_review_wait and not is_subagent then
        observers.on_review_wait=function(...)
            -- Review retains its run/cancellation/telemetry, not the input slot.
            opts.background=true
            background_runs[cancel]=opts
            if _G.agent_cancel_root==cancel then
                _G.agent_root_pending=false
                _G.agent_cancel_root=nil
            end
            callbacks.on_review_wait(...)
        end
        observers.on_review_resume=function()
            opts.background=false
            background_runs[cancel]=nil
            _G.agent_root_pending=true
            _G.agent_cancel_root=cancel
            if callbacks.on_review_resume then callbacks.on_review_resume() end
        end
    end
    if opts.background then background_runs[cancel] = opts end
    if not is_subagent then
        _G.agent_root_pending=true
        _G.agent_cancel_root=cancel
    end
    observers.on_done = function(result)
        if settled then return end
        settled = true
        background_runs[cancel] = nil
        root_polls[opts]=nil
        if _G.agent_cancel_root==cancel then
            _G.agent_root_pending=false
            _G.agent_cancel_root=nil
        end
        if opts.review_cleanup then
            local cleanup=opts.review_cleanup; opts.review_cleanup=nil
            cleanup(result)
        end
        result = result or {}
        if type(result.duration_ms) == "number" then
            local residual = result.duration_ms - measurements.model_ms - measurements.tool_ms
                - measurements.permission_wait_ms - measurements.subagent_wait_ms
            measurements.unattributed_ms = math.max(0, residual)
            measurements.overlap_ms = math.max(0, -residual)
        end
        result.measurements = copy_table(measurements)
        local attributes = copy_table(measurements)
        attributes["error.category"] = result.error_category
        attributes.turns, attributes.duration_ms = result.turns, result.duration_ms
        local closed, close_error = pcall(telemetry.finish, run_span, result.ok ~= false,
            result.cancelled == true or result.error == "cancelled", attributes)
        local delivered, delivery_error = true, nil
        if callbacks.on_done then
            delivered, delivery_error = pcall(callbacks.on_done, result, run_span.context)
        end
        if not closed then error(close_error, 0) end
        if not delivered then error(delivery_error, 0) end
    end
    run_span.finish_exception = function(err)
        observers.on_done({ok = false, error = err, error_category = "exception", text = ""})
    end
    run_span.on_terminal = function(_, cancelled)
        observers.on_done({ok = false, cancelled = cancelled,
            error = run_span.terminal_error or (cancelled and "cancelled" or "owner finished"), text = ""})
    end
    if run_span.ended then
        run_span.on_terminal(false, true)
        run_span.on_terminal = nil
        return false, "cancelled"
    end
    local ok, err = protect_run(run_span, function()
        return in_context(opts, function()
        -- Deliver explicit correlation before validation, hooks, or transport.
        -- Like other API observers, failures terminate the owned run.
        if type(callbacks.on_run_start) == "function" then
            callbacks.on_run_start(run_span.context)
        end
        if run_span.ended then return false, "cancelled" end
        return run_impl(opts, observers, run_span)
        end)
    end)
    return ok, err, cancel
end

-- Inherit only model routing; background work keeps its own run policy.
local function interactive_model_options(opts)
    local selected = interactive_options()
    if opts.reasoning_effort == nil and opts.reasoning_effort_default == nil then
        opts.reasoning_effort = selected.reasoning_effort
        opts.reasoning_effort_default = selected.reasoning_effort_default
    end
    for _, field in ipairs({"provider", "model"}) do
        if opts[field] == nil then opts[field] = selected[field] end
    end
    return opts
end

if not _G.capstan then _G.capstan = {} end
_G.capstan.agent = {
    run = function(opts, callbacks)
        opts = copy_table(opts) or {}
        -- Explicit model/provider/profile selections own their routing. Default
        -- background callers (including Wiki ingest) inherit the active model.
        if not opts.provider and not opts.model and not opts.profile then
            interactive_model_options(opts)
        end
        return M.run(opts, callbacks)
    end,
    configure_interactive = function(opts)
        opts = type(opts) == "table" and opts or {}
        if opts.provider and not M.providers[opts.provider] then
            return nil, "unknown provider: " .. tostring(opts.provider)
        end
        if opts.profile and not profiles.normalize(opts.profile) then
            return nil, "unknown profile: " .. tostring(opts.profile)
        end
        interactive_run_options = {}
        interactive_effort_profiles = {}
        if opts.profile then active_profile_name = profiles.normalize(opts.profile) end
        local fields = {
            "provider", "model", "reasoning_effort", "max_turns",
            "preserve_reasoning",
        }
        for _, field in ipairs(fields) do
            if opts[field] ~= nil then
                interactive_run_options[field] = opts[field]
            end
        end
        publish_agent_status()
        return true
    end,
    step_reasoning_effort = function(direction)
        local info = effective_model_info()
        if not info or not info.profile then return nil, "No active model/profile" end
        local next_effort, err = models.step_reasoning_effort(
            models.cached_reasoning_efforts(M, info.provider, info.model),
            info.reasoning_effort, direction)
        if not next_effort then return nil, err end
        if next_effort == (info.reasoning_effort or "default") then return next_effort end
        local ok, save_err = models.set_profile(M, info.profile, info.provider, info.model, next_effort)
        interactive_effort_profiles[info.profile] = true
        publish_agent_status()
        if not ok then return nil, "Effort changed but could not be saved: " .. tostring(save_err) end
        return next_effort
    end,
    set_profile = function(name)
        local normalized = profiles.normalize(name)
        if not normalized then return nil, "unknown profile" end
        active_profile_name = normalized
        publish_agent_status(normalized)
        return normalized
    end,
    get_profile = function()
        return active_profile_name or configured_profile() or profiles.default_name()
    end,
    clear_profile = function()
        active_profile_name = nil
        publish_agent_status()
    end,
    refresh_status = function()
        publish_agent_status()
    end,
    profiles = function()
        return profiles.names()
    end,
}

_G.capstan.models.effective = function(profile_name)
    local info = effective_model_info(profile_name)
    return info
end

_G.capstan.agent.reasoning_effort = function(profile_name)
    local profile = profiles.get(profile_name) or effective_profile({})
    local active = prepare_provider({}, profile)
    return effective_reasoning_effort(active, nil, profile)
end

_G.capstan.mcp = {
    tick = function(max_steps)
        return mcp_client.tick(max_steps)
    end,
}

publish_agent_status()

local AUTO_COMPACT_DEFAULT_PERCENT = 80

local function auto_compact_percent()
    local configured = config_table("agent")
    local value = configured and tonumber(configured.auto_compact_percent)
    if value == nil then return AUTO_COMPACT_DEFAULT_PERCENT end
    value = math.floor(value)
    if value <= 0 then return 0 end
    return math.min(value, 100)
end

-- Called by the TUI dispatcher before it appends a new user submission. This
-- mirrors the normal request's system/profile/tool assembly without running
-- user hooks, which must not gain an extra side-effecting invocation merely
-- because a budget check occurred.
_G.should_auto_compact = function(messages, additional_text)
    local threshold = auto_compact_percent()
    if threshold == 0 or not messages or #messages == 0 then
        return false, 0, 0, threshold
    end

    local profile = effective_profile(interactive_run_options)
    local active, provider_name = prepare_provider(interactive_run_options, profile)
    if not active then
        logging.runtime_log("compact",
            "auto_check skipped unknown_provider=" .. tostring(provider_name),
            "warn")
        return false, 0, 0, threshold
    end
    local context_limit = models.ensure_context_limit(active)
    if not context_limit or context_limit <= 0 then
        logging.runtime_log("compact",
            "auto_check skipped context_limit=unknown")
        return false, 0, 0, threshold
    end

    local candidate = {}
    for _, message in ipairs(messages) do
        table.insert(candidate, message)
    end
    if type(additional_text) == "string" and additional_text ~= "" then
        table.insert(candidate, {role = "user", content = additional_text})
    end

    local request_messages = build_messages(candidate, profile)
    local request_tools = tools_runtime.collect()
    request_tools = profiles.filter_tools(request_tools, profile)
    local estimated_tokens =
        tokens.estimate_messages_tokens(request_messages, request_tools)
    local percent = estimated_tokens * 100 / context_limit
    local trigger = percent >= threshold
    logging.runtime_log("compact", string.format(
        "auto_check estimated_tokens=%d context_limit=%d percent=%.1f threshold=%d trigger=%s",
        estimated_tokens,
        context_limit,
        percent,
        threshold,
        tostring(trigger)
    ))
    return trigger, estimated_tokens, context_limit, threshold
end

local compact_instruction = [[
Compact the conversation above into an operational handoff summary for a coding agent.

Preserve only information needed to continue the work correctly:
- current user goal and latest instruction overrides;
- repository/project constraints and conventions;
- files inspected or changed, with important paths;
- commands run and verification results;
- decisions made and why;
- pending TODOs, blockers, and risks;
- user changes that must not be reverted.

Do not write a conversational recap. Do not omit concrete file names, model/provider choices, test results, or unresolved work. Use concise Markdown.
]]

local function compact_run_options(messages)
    local compact_messages = {}
    for _, message in ipairs(messages or {}) do
        table.insert(compact_messages, message)
    end
    table.insert(compact_messages, {
        role = "user",
        content = compact_instruction,
    })

    local opts = {
        messages = compact_messages,
        purpose = "compaction",
        max_turns = 1,
        tools = {},
        silent_tools = true,
        update_status = false,
        update_usage = false,
        skip_after_agent_turn = true,
    }
    return interactive_model_options(opts)
end

_G.compact_entry = function(messages)
    interactive_generation=interactive_generation+1
    if not messages or #messages == 0 then
        popup.error("Compact", "No conversation to compact")
        return
    end

    local session_id = type(agent.session_id) == "function" and agent.session_id() or nil
    local chunks = {}
    local opts = compact_run_options(messages)
    agent.set_activity("Compacting")
    agent.set_thinking(true)
    M.run(opts, {
        on_text = function(chunk)
            table.insert(chunks, chunk)
        end,
        on_error = function(message)
            agent.set_thinking(false)
            agent.set_activity(nil)
            popup.error("Compact", message)
        end,
        on_done = function(result, run_context)
            agent.set_thinking(false)
            agent.set_activity(nil)
            if not result or result.ok == false then
                agent.finish_run(run_context, session_id)
                local message = result and result.error or "compact failed"
                popup.error("Compact", message)
                return
            end
            local text = result.text or table.concat(chunks)
            text = text:gsub("^%s+", ""):gsub("%s+$", "")
            if text == "" then
                agent.finish_run(run_context, session_id)
                popup.error("Compact", "Compact returned an empty summary")
                return
            end
            agent.replace_compacted_context(text)
            agent.finish_run(run_context, session_id)
        end,
    })
end

local session_title_instruction = [[
Create a concise title for this conversation in the same language as the user.
Use 3 to 7 words. Return only the title: no quotes, markdown, punctuation suffix,
or explanation.
]]

local session_title_jobs = {}

local function generate_session_title(parent_context, owner_session_id)
    if type(agent.session_title_context) ~= "function" or
       type(agent.set_session_title) ~= "function" then
        return
    end
    local session_id, user_text, assistant_text = agent.session_title_context()
    if not session_id or session_title_jobs[session_id] then return end
    if owner_session_id and session_id ~= owner_session_id then return end
    session_title_jobs[session_id] = true

    local opts = {
        messages = {{
            role = "user",
            content = session_title_instruction ..
                "\nUser message:\n" .. user_text ..
                "\n\nAssistant response:\n" .. assistant_text,
        }},
        max_turns = 1,
        tools = {},
        background = true,
        purpose = "title",
        telemetry_parent = parent_context,
        silent_tools = true,
        update_status = false,
        update_usage = false,
        skip_after_agent_turn = true,
    }

    M.run(interactive_model_options(opts), {
        -- Title generation is metadata work. Supplying an explicit text
        -- callback prevents M.run's UI-visible agent.append fallback.
        on_text = function() end,
        on_done = function(result, run_context)
            session_title_jobs[session_id] = nil
            if not result or result.ok == false then return end
            local title = tostring(result.text or "")
            title = title:gsub("^%s+", ""):gsub("%s+$", "")
            title = title:gsub("^[\"'`]+", ""):gsub("[\"'`]+$", "")
            if title ~= "" then agent.set_session_title(session_id, title, run_context) end
        end,
    })
end

local interactive_permission_scopes = {}

local function interactive_permission_scope()
    local session_id = type(agent.session_id) == "function" and agent.session_id() or "interactive"
    session_id = tostring(session_id or "interactive")
    if not interactive_permission_scopes[session_id] then
        interactive_permission_scopes[session_id] = {
            allowed_tools = {},
            allowed_targets = {},
            full_control = false,
            yolo = yolo_enabled,
            workdir_only = false,
        }
    end
    return interactive_permission_scopes[session_id]
end

_G.capstan.agent.set_yolo = function(enabled)
    yolo_enabled = enabled == true
    for _, scope in pairs(interactive_permission_scopes) do
        scope.yolo = yolo_enabled
    end
end

-- Entry point called from C via agent_build_and_dispatch. Receives message
-- history as a Lua table, runs the full agent cycle with UI-visible streaming.
_G.agent_entry = function(messages)
    interactive_generation=interactive_generation+1
    local generation=interactive_generation
    local session_id = type(agent.session_id) == "function" and agent.session_id() or nil
    local output_sink=agent.output_sink and agent.output_sink()
    local sink_is_segment = output_sink ~= nil
    local sink=output_sink or function(text)
        if text then agent.append(text,'agent') end
        return true
    end
    local function current()
        return generation==interactive_generation and sink() and
            (not agent.session_id or agent.session_id()==session_id)
    end
    local opts = {
        messages = messages,
        update_status = true,
        update_usage = true,
        permission_scope = interactive_permission_scope(),
        review_foreground_lost=function() return not current() end,
        is_cancelled=function()
            return not sink() or (agent.session_id and agent.session_id()~=session_id)
        end,
    }
    for field, value in pairs(interactive_options()) do
        opts[field] = value
    end
    local review_id
    M.run(opts, {
        on_text = function(text) return sink(text) end,
        on_review_result = function(text)
            if agent.session_id and agent.session_id() ~= session_id then return end
            if agent.review_event then
                agent.review_event('Background review result ('..tostring(review_id or 'existing review')..'):\n'..text)
            else sink(text) end
        end,
        on_review_wait = function(id, cycle)
            review_id = id
            if agent.review_event then
                agent.review_event('Review '..tostring(id)..(cycle > 0 and ' requeued' or ' started')..
                    ' in background. Results will arrive later in this conversation.')
            end
            agent.set_running(false)
            agent.set_thinking(false)
            agent.set_activity(nil)
        end,
        on_review_resume = function()
            -- Repair prose is a new chronological segment after the review
            -- marker, not a continuation of the pre-review answer.
            if agent.output_sink then
                sink = assert(agent.output_sink(true))
                sink_is_segment = true
            end
            agent.set_running(true)
        end,
        on_error = function(message)
            if current() then popup.error("Provider", message) end
        end,
        on_done = function(result, run_context)
            if not current() then return end
            agent.set_thinking(false)
            agent.set_activity(nil)
            agent.finish_run(run_context, session_id)
            if result and result.ok ~= false then
                generate_session_title(run_context, session_id)
            end
        end,
    })
end

return M
