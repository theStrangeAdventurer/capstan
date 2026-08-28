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

function M.set_weak_model(provider_name, model, reasoning_effort)
    return models.set_weak(M, provider_name, model, reasoning_effort)
end

function M.set_profile_model(profile_name, provider_name, model, reasoning_effort)
    return models.set_profile(M, profile_name, provider_name, model, reasoning_effort)
end

function M.get_weak_model()
    return models.weak(M)
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

local function configured_profile()
    local configured = config_table("agent")
    local from_agent = configured and profiles.normalize(configured.profile)
    if from_agent then return from_agent end
    return profiles.normalize(_G.capstan and _G.capstan.config and _G.capstan.config.profile)
end

local function effective_profile(opts)
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
local function build_messages(messages, profile)
    local msgs = {}
    local system = _G.system_prompt or ""
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

local function request_reasoning(provider, effort)
    local reasoning = copy_table(provider and provider.reasoning) or {}
    local provider_effort = normalize_reasoning_effort(provider and provider.reasoning_effort)
    if provider_effort then reasoning.effort = provider_effort end
    if effort then reasoning.effort = effort end
    if provider and provider.reasoning_max_tokens then
        reasoning.max_tokens = provider.reasoning_max_tokens
    end
    if provider and provider.reasoning_exclude ~= nil then
        reasoning.exclude = provider.reasoning_exclude and true or false
    end
    return next(reasoning) and reasoning or nil
end

local function apply_request_reasoning(request, provider, effort)
    local reasoning = request_reasoning(provider, effort)
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
    if not opts.provider and not opts.model and profile and not M.env_provider_override then
        profile_model = models.profile(M, profile.name)
        if profile_model then
            provider_name = profile_model.provider
        end
    end
    local active = M.providers[provider_name]
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

local function effective_model_info(profile_name)
    local profile = profiles.get(profile_name) or effective_profile(interactive_run_options)
    local active, provider_name = prepare_provider(interactive_run_options, profile)
    if not active then
        return nil, provider_name
    end
    return {
        provider = provider_name,
        model = active.model,
        reasoning_effort = effective_reasoning_effort(active, interactive_run_options, profile),
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

local completion_review_instruction = [[
Before finalizing, perform one bounded completion review of this implementation.
Re-read the original request line by line. For every required behavior and each
exact term, name, version, value, or scope, locate evidence in the changed source
or a direct check at the boundary where consumers observe it. A related effect,
default, label, visual approximation, or setting at another hierarchy level is
not evidence. Fix any concrete gap and validate that distinct requirement;
otherwise give the final answer. Do not repeat checks or add dependencies or an
ad-hoc harness solely for this review.
]]

local empty_terminal_instruction = [[
Continue from the available tool results. If the task is complete, provide a
concise final result and validation summary. Otherwise perform the next
necessary action. Mention any blocker or remaining uncertainty, and never
return an empty response.
]]

local function completion_review_enabled(opts, profile, run_depth)
    if run_depth > 0 then return false end
    if type(opts.completion_review) == "boolean" then return opts.completion_review end
    local configured = agent_config_boolean("completion_review")
    if configured ~= nil then return configured end
    return profile and profile.completion_review == true
end

local function preserve_reasoning_enabled(opts)
    if opts and opts.preserve_reasoning ~= nil then
        return opts.preserve_reasoning ~= false
    end
    local configured = agent_config_boolean("preserve_reasoning")
    if configured ~= nil then return configured end
    return true
end

local function completion_review_warranted(state)
    if not state or not state.workspace_mutated then return false end
    if state.successful_validation then return false end
    local targets = state.workspace_write_targets or {}
    local count = 0
    for _ in pairs(targets) do
        count = count + 1
        if count >= 2 then return true end
    end
    return false
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
        max_generated_output_checks = agent_config_nonnegative("max_generated_output_checks", 0),
        total_tool_calls = 0,
        generated_output_checks = 0,
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

-- Full agent run: build messages, stream LLM response, handle tool_calls recursively.
function M.run(opts, callbacks)
    opts = opts or {}
    callbacks = callbacks or {}
    if opts.profile ~= nil and not profiles.normalize(opts.profile) then
        local message = "Unknown profile: " .. tostring(opts.profile)
        if callbacks.on_error then callbacks.on_error(message) end
        if callbacks.on_done then callbacks.on_done({ok = false, error = message, text = ""}) end
        return false, message
    end
    local profile = effective_profile(opts)
    local active, provider_name = prepare_provider(opts, profile)
    if not active then
        local message = "Unknown provider: " .. tostring(provider_name)
        logging.runtime_log("provider", "unknown provider: " .. tostring(provider_name))
        if callbacks.on_error then callbacks.on_error(message) end
        if callbacks.on_done then callbacks.on_done({ok = false, error = message, text = ""}) end
        return false, message
    end

    local effort = effective_reasoning_effort(active, opts, profile)
    if opts.update_status ~= false then
        agent.set_info(provider_name, active.model, effort or "default")
        if type(agent.set_profile_info) == "function" then
            agent.set_profile_info(display_profile_name(profile))
        end
    end
    models.ensure_context_limit(active)
    if opts.update_usage ~= false then
        agent.set_usage(0, 0, 0, active.context_limit or 0)
    end
    local msgs = build_messages(opts.messages or {}, profile)
    local messages_ctx = hooks.run("before_messages", {
        runtime = M,
        provider = active,
        provider_name = provider_name,
        messages = msgs,
        run = opts,
    })
    msgs = messages_ctx.messages or msgs

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
        run = opts,
    })
    combined_tools = tools_ctx.tools or combined_tools
    combined_tools = profiles.filter_tools(combined_tools, profile)
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
    local review_enabled = completion_review_enabled(opts, profile, run_depth)
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
        if type(opts.is_cancelled) ~= "function" then return false end
        local ok, cancelled = pcall(opts.is_cancelled)
        return not ok or cancelled == true
    end

    local function stop_run(message, current_msgs)
        if finished then return end
        finished = true
        logging.runtime_log("tool_guard", logging.compact(message, 500))
        if agent and type(agent.append) == "function" and opts.silent_tools ~= true then
            ui.append("\n[stopped: " .. message .. "]\n")
        end
        if callbacks.on_error then callbacks.on_error(message) end
        finish({ok = false, error = message, text = "", messages = current_msgs or {}, turns = turns})
    end

    -- One turn of the agent cycle: sends the request, streams the response,
    -- and either finishes or recurses into handle_tool_calls.
    local function continue_agent_cycle(current_msgs, tools, cycle_kind)
        cycle_kind = cycle_kind or "agent"
        if finished then return end
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

        local deferred_text_chunks = {}
        local defer_visible_text = review_enabled and run_state.workspace_mutated
        local stream_attempt = 0
        local stream_emitted_text = false
        local model_started_at = nil
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

        local function on_result(result, is_done)
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
                    if defer_visible_text then
                        table.insert(deferred_text_chunks, result.content)
                    else
                        publish_text(result.content)
                    end
                end
                return
            end

            if type(callbacks.on_model_done) == "function" then
                local observer_ok, observer_error = pcall(
                    callbacks.on_model_done,
                    turns,
                    stream_attempt,
                    result.ok ~= false,
                    math.max(0, math.floor(now_ms() - (model_started_at or now_ms()))),
                    #(result.text or ""),
                    #(result.reasoning or ""),
                    #(result.tool_calls or {}),
                    result.metrics)
                if not observer_ok then
                    stop_run(
                        "observability callback on_model_done failed: " ..
                            tostring(observer_error),
                        current_msgs)
                    return
                end
            end

            if result.ok == false then
                local message = result.error or "agent stream failed"
                if not stream_emitted_text and stream_attempt <= max_stream_retries and
                    retryable_stream_error(message) then
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
                if run_state.completion_review_fallback then
                    local fallback = run_state.completion_review_fallback
                    logging.runtime_log("agent", "completion_review failed; preserving prior answer")
                    publish_text(fallback.text)
                    finished = true
                    finish({
                        ok = true,
                        text = fallback.text,
                        messages = fallback.messages,
                        turns = turns,
                        provider = provider_name,
                        model = active.model,
                        review_error = message,
                    })
                    return
                end
                if callbacks.on_error then callbacks.on_error(message) end
                finished = true
                finish({ok = false, error = message, text = result.text or ""})
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
                if defer_visible_text and #deferred_text_chunks > 0 then
                    publish_text(table.concat(deferred_text_chunks))
                    deferred_text_chunks = {}
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
                    provider = active,
                    provider_name = provider_name,
                    depth = tonumber(opts.depth) or 0,
                    max_turns = max_turns,
                    profile = profile and profile.name or nil,
                    tools = tools,
                    silent_tools = opts.silent_tools,
                    update_status = opts.update_status ~= false,
                    permission_scope = permission_scope,
                    mcp_scope = opts.mcp_scope,
                    callbacks = callbacks,
                    guard = guard,
                    state = run_state,
                    assistant_reasoning = preserve_reasoning and result.reasoning or nil,
                    assistant_reasoning_details = preserve_reasoning and result.reasoning_details or nil,
                    assistant_reasoning_field = active.reasoning_history_field,
                    stop_run = stop_run,
                })
            else
                local final_text = result.text or table.concat(deferred_text_chunks)
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
                if review_enabled and completion_review_warranted(run_state) and not run_state.completion_review_done then
                    run_state.completion_review_done = true
                    local draft = final_text
                    run_state.completion_review_fallback = {
                        text = draft,
                        messages = current_msgs,
                    }
                    table.insert(current_msgs, {role = "assistant", content = draft})
                    table.insert(current_msgs, {role = "user", content = completion_review_instruction})
                    logging.runtime_log("agent", "completion_review started")
                    publish_status("\n⚙ Completion review\n\n")
                    continue_agent_cycle(current_msgs, tools, "completion_review")
                    return
                end
                if defer_visible_text then
                    publish_text(final_text)
                end
                if opts.skip_after_agent_turn ~= true then
                    hooks.run("after_agent_turn", {
                        runtime = M,
                        provider = active,
                        provider_name = provider_name,
                        messages = current_msgs,
                        tools = tools,
                        text = final_text,
                        run = opts,
                    })
                end
                finished = true
                finish({
                    ok = true,
                    text = final_text,
                    messages = current_msgs,
                    turns = turns,
                    provider = provider_name,
                    model = active.model,
                })
            end
        end

        local prompt_estimate = tokens.estimate_messages_tokens(current_msgs, tools)
        if opts.update_usage ~= false then
            agent.set_usage(
                prompt_estimate,
                0,
                prompt_estimate,
                active.context_limit or 0
            )
        end

        local request = {
            model = active.model,
            messages = current_msgs,
            tools = #tools > 0 and tools or nil,
            tool_choice = #tools > 0 and "auto" or nil,
            stream = true,
            stream_options = {include_usage = true}
        }
        apply_request_reasoning(request, active, effort)

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
            run = opts,
        })
        request = request_ctx.request or request
        headers = request_ctx.headers or headers
        local endpoint = request_ctx.endpoint or active.endpoint
        if request_ctx.error then
            local message = tostring(request_ctx.error)
            if opts.update_usage ~= false then
                agent.set_info("error", message)
            end
            if callbacks.on_error then callbacks.on_error(message) end
            if callbacks.on_done then callbacks.on_done({ok = false, error = message, text = ""}) end
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
            stream_attempt = stream_attempt + 1
            model_started_at = now_ms()
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
                        current_msgs)
                    return
                end
            end
            local response_callback = stream.stream(
                active, on_result, prompt_estimate, opts)
            local transport_ok, transport_error = pcall(
                http.post_stream,
                endpoint, body, headers, response_callback,
                stream_timeout_sec * 1000,
                {background = opts.background == true})
            if not transport_ok then
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

if not _G.capstan then _G.capstan = {} end
_G.capstan.agent = {
    run = function(opts, callbacks)
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

    local profile = effective_profile({})
    local active, provider_name = prepare_provider({}, profile)
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

local function compact_weak_model_fits(weak, compact_messages, automatic)
    if not weak then return false end
    local profile = effective_profile({})
    local active = prepare_provider({
        provider = weak.provider,
        model = weak.model,
        update_status = false,
        update_usage = false,
    }, profile)
    if not active then return false end
    local context_limit = models.ensure_context_limit(active)
    if not context_limit or context_limit <= 0 then
        if automatic then
            logging.runtime_log("compact", string.format(
                "weak_model_skipped context_limit=unknown provider=%s model=%s",
                tostring(weak.provider),
                tostring(weak.model)
            ), "warn")
            return false
        end
        return true
    end
    local request_messages = build_messages(compact_messages, profile)
    local estimated_tokens = tokens.estimate_messages_tokens(request_messages, {})
    local fits = estimated_tokens * 100 < context_limit * 90
    if not fits then
        logging.runtime_log("compact", string.format(
            "weak_model_skipped estimated_tokens=%d context_limit=%d provider=%s model=%s",
            estimated_tokens,
            context_limit,
            tostring(weak.provider),
            tostring(weak.model)
        ), "warn")
    end
    return fits
end

local function compact_run_options(messages, automatic)
    local compact_messages = {}
    for _, message in ipairs(messages or {}) do
        table.insert(compact_messages, message)
    end
    table.insert(compact_messages, {
        role = "user",
        content = compact_instruction,
    })

    local weak = models.weak(M)
    if weak and
       not compact_weak_model_fits(weak, compact_messages, automatic) then
        weak = nil
    end
    local opts = {
        messages = compact_messages,
        max_turns = 1,
        tools = {},
        silent_tools = true,
        update_status = false,
        update_usage = false,
        skip_after_agent_turn = true,
    }
    if weak then
        opts.provider = weak.provider
        opts.model = weak.model
        if weak.reasoning_effort == "default" then
            opts.reasoning_effort_default = true
        else
            opts.reasoning_effort = weak.reasoning_effort
        end
    end
    return opts, weak
end

_G.compact_entry = function(messages, automatic)
    if not messages or #messages == 0 then
        popup.error("Compact", "No conversation to compact")
        return
    end

    local chunks = {}
    local opts, weak = compact_run_options(messages, automatic == true)
    agent.set_activity(weak and "Compacting" or "Compacting")
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
        on_done = function(result)
            agent.set_thinking(false)
            agent.set_activity(nil)
            if not result or result.ok == false then
                agent.finish_run()
                local message = result and result.error or "compact failed"
                popup.error("Compact", message)
                return
            end
            local text = result.text or table.concat(chunks)
            text = text:gsub("^%s+", ""):gsub("%s+$", "")
            if text == "" then
                agent.finish_run()
                popup.error("Compact", "Compact returned an empty summary")
                return
            end
            agent.replace_compacted_context(text)
            agent.finish_run()
        end,
    })
end

local session_title_instruction = [[
Create a concise title for this conversation in the same language as the user.
Use 3 to 7 words. Return only the title: no quotes, markdown, punctuation suffix,
or explanation.
]]

local session_title_jobs = {}

local function generate_session_title()
    if type(agent.session_title_context) ~= "function" or
       type(agent.set_session_title) ~= "function" then
        return
    end
    local session_id, user_text, assistant_text = agent.session_title_context()
    if not session_id or session_title_jobs[session_id] then return end
    session_title_jobs[session_id] = true

    local weak = models.weak(M)
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
        silent_tools = true,
        update_status = false,
        update_usage = false,
        skip_after_agent_turn = true,
    }
    if weak then
        opts.provider = weak.provider
        opts.model = weak.model
        if weak.reasoning_effort == "default" then
            opts.reasoning_effort_default = true
        else
            opts.reasoning_effort = weak.reasoning_effort
        end
    end

    M.run(opts, {
        -- Title generation is metadata work. Supplying an explicit text
        -- callback prevents M.run's UI-visible agent.append fallback.
        on_text = function() end,
        on_done = function(result)
            session_title_jobs[session_id] = nil
            if not result or result.ok == false then return end
            local title = tostring(result.text or "")
            title = title:gsub("^%s+", ""):gsub("%s+$", "")
            title = title:gsub("^[\"'`]+", ""):gsub("[\"'`]+$", "")
            if title ~= "" then agent.set_session_title(session_id, title) end
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
    local opts = {
        messages = messages,
        update_status = true,
        update_usage = true,
        permission_scope = interactive_permission_scope(),
    }
    for field, value in pairs(interactive_run_options) do
        opts[field] = value
    end
    M.run(opts, {
        on_text = function(chunk)
            agent.append(chunk, "agent")
        end,
        on_error = function(message)
            popup.error("Provider", message)
        end,
        on_done = function(result)
            agent.set_thinking(false)
            agent.set_activity(nil)
            agent.finish_run()
            if result and result.ok ~= false then
                generate_session_title()
            end
        end,
    })
end

return M
