local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local models = require("agent.models")
local mcp_client = require("agent.mcp")
local provider_config = require("agent.provider_config")
local profiles = require("agent.profiles")
local stream = require("agent.stream")
local tokens = require("agent.tokens")
local tools_runtime = require("agent.tools")
local workspace = require("agent.workspace")

local M = provider_config.build()

M.parse_sse_event = stream.parse_sse_event
hooks.install_config((_G.capstan and _G.capstan.config) or {})
hooks.install_existing_plugins(_G.plugins)

local runtime_options = (_G.capstan and _G.capstan.runtime_options) or {}
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

function M.set_model(provider_name, model)
    return models.set(M, provider_name or M.provider, model)
end

function M.set_weak_model(provider_name, model)
    return models.set_weak(M, provider_name, model)
end

function M.set_profile_model(profile_name, provider_name, model)
    return models.set_profile(M, profile_name, provider_name, model)
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
    if not _G.capstan or type(_G.capstan.config) ~= "table" then return nil end
    local value = _G.capstan.config[name]
    return type(value) == "table" and value or nil
end

local active_profile_name = nil

local function configured_profile()
    local configured = config_table("agent")
    local from_agent = configured and profiles.normalize(configured.profile)
    if from_agent then return from_agent end
    return profiles.normalize(_G.capstan and _G.capstan.config and _G.capstan.config.profile)
end

local function effective_profile(opts)
    local name = profiles.normalize(opts and opts.profile) or active_profile_name or configured_profile() or "implement"
    return profiles.get(name)
end

-- Assembles the message list: prepends system_prompt, then copies all messages.
local function build_messages(messages, profile)
    local msgs = {}
    local system = _G.system_prompt or ""
    if profile and profile.prompt then
        system = system .. "\n\n" .. profile.prompt
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
    return normalize_reasoning_effort(opts and opts.reasoning_effort) or
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
        copy.context_limit = 0
        active = copy
    end
    if opts.model and opts.model ~= "" then
        local copy = {}
        for k, v in pairs(active) do copy[k] = v end
        copy.model = opts.model
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
    local profile = profiles.get(profile_name) or effective_profile({})
    local active, provider_name = prepare_provider({}, profile)
    if not active then
        return nil, provider_name
    end
    return { provider = provider_name, model = active.model, profile = profile and profile.name or nil }, nil
end

local function display_profile_name(profile)
    return profile and profile.name or nil
end

local function publish_agent_status(profile_name)
    local info = effective_model_info(profile_name)
    if info and agent then
        if type(agent.set_info) == "function" then
            agent.set_info(info.provider, info.model)
        end
        if type(agent.set_profile_info) == "function" then
            agent.set_profile_info(info.profile)
        end
    end
    return info
end

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

local function completion_review_enabled(opts, profile, run_depth)
    if run_depth > 0 then return false end
    if type(opts.completion_review) == "boolean" then return opts.completion_review end
    local configured = agent_config_boolean("completion_review")
    if configured ~= nil then return configured end
    return profile and profile.completion_review == true
end

local function completion_review_warranted(state)
    if not state or not state.workspace_mutated then return false end
    if state.successful_validation then return true end
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

local function make_guard(started_at)
    return {
        started_at = started_at,
        max_duration_ms = agent_config_number("max_duration_sec", 900) * 1000,
        max_tool_calls = agent_config_number("max_tool_calls", 80),
        max_same_tool_call = agent_config_number("max_same_tool_call", 3),
        max_same_shell_command = agent_config_number("max_same_shell_command", 0),
        max_generated_output_checks = agent_config_nonnegative("max_generated_output_checks", 1),
        total_tool_calls = 0,
        generated_output_checks = 0,
        signatures = {},
        last_tool_signature = nil,
        same_tool_count = 0,
        last_shell_signature = nil,
        same_shell_count = 0,
    }
end

local function guard_duration_error(guard)
    if guard.max_duration_ms > 0 and now_ms() - guard.started_at > guard.max_duration_ms then
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
    local profile = effective_profile(opts)
    local active, provider_name = prepare_provider(opts, profile)
    if not active then
        local message = "Unknown provider: " .. tostring(provider_name)
        logging.runtime_log("provider", "unknown provider: " .. tostring(provider_name))
        if callbacks.on_error then callbacks.on_error(message) end
        if callbacks.on_done then callbacks.on_done({ok = false, error = message, text = ""}) end
        return false, message
    end

    if opts.update_status ~= false then
        agent.set_info(provider_name, active.model)
        if type(agent.set_profile_info) == "function" then
            agent.set_profile_info(display_profile_name(profile))
        end
    end
    models.ensure_context_limit(active)
    if opts.update_usage ~= false then
        agent.set_usage(0, 0, 0, active.context_limit or 0)
    end

    local effort = effective_reasoning_effort(active, opts, profile)
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
    local stream_timeout_sec = agent_config_number("stream_timeout_sec", 120)
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
    }
    local review_enabled = completion_review_enabled(opts, profile, run_depth)
    local permission_scope = opts.permission_scope or {allowed_tools = {}, full_control = false}
    if type(permission_scope.allowed_tools) ~= "table" then
        permission_scope.allowed_tools = {}
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

    local function stop_run(message, current_msgs)
        if finished then return end
        finished = true
        logging.runtime_log("tool_guard", logging.compact(message, 500))
        if agent and type(agent.append) == "function" and opts.silent_tools ~= true then
            agent.append("\n[stopped: " .. message .. "]\n", "agent")
        end
        if callbacks.on_error then callbacks.on_error(message) end
        finish({ok = false, error = message, text = "", messages = current_msgs or {}, turns = turns})
    end

    -- One turn of the agent cycle: sends the request, streams the response,
    -- and either finishes or recurses into handle_tool_calls.
    local function continue_agent_cycle(current_msgs, tools)
        if finished then return end
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
        local start_stream

        local function publish_text(text)
            if text == "" then return end
            if callbacks.on_text then
                callbacks.on_text(text)
            else
                agent.append(text, "agent")
            end
        end

        local function on_result(result, is_done)
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
                    permission_scope = permission_scope,
                    callbacks = callbacks,
                    guard = guard,
                    state = run_state,
                    stop_run = stop_run,
                })
            else
                if (result.text or "") == "" then
                    logging.runtime_log("agent", "stream completed with no text and no tool calls", "warn")
                else
                    logging.runtime_log("agent", "stream done without tool calls text=" .. logging.compact(result.text, 500))
                end
                if review_enabled and completion_review_warranted(run_state) and not run_state.completion_review_done then
                    run_state.completion_review_done = true
                    local draft = result.text or table.concat(deferred_text_chunks)
                    run_state.completion_review_fallback = {
                        text = draft,
                        messages = current_msgs,
                    }
                    table.insert(current_msgs, {role = "assistant", content = draft})
                    table.insert(current_msgs, {role = "user", content = completion_review_instruction})
                    logging.runtime_log("agent", "completion_review started")
                    continue_agent_cycle(current_msgs, tools)
                    return
                end
                if defer_visible_text then
                    publish_text(result.text or table.concat(deferred_text_chunks))
                end
                hooks.run("after_agent_turn", {
                    runtime = M,
                    provider = active,
                    provider_name = provider_name,
                    messages = current_msgs,
                    tools = tools,
                    text = result.text or "",
                    run = opts,
                })
                finished = true
                finish({
                    ok = true,
                    text = result.text or "",
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
        local reasoning = request_reasoning(active, effort)
        if reasoning then
            request.reasoning = reasoning
        end

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
        local body = json.encode(request)

        logging.runtime_log("api", string.format("post_stream endpoint=%s messages=%d tools=%d",
            endpoint or "",
            #current_msgs,
            #tools
        ))

        start_stream = function()
            stream_attempt = stream_attempt + 1
            http.post_stream(endpoint, body, headers,
                stream.stream(active, on_result, prompt_estimate, opts),
                stream_timeout_sec * 1000)
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
    set_profile = function(name)
        local normalized = profiles.normalize(name)
        if not normalized then return nil, "unknown profile" end
        active_profile_name = normalized
        publish_agent_status(normalized)
        return normalized
    end,
    get_profile = function()
        return active_profile_name or configured_profile() or "implement"
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

    local weak = models.weak(M)
    local opts = {
        messages = compact_messages,
        max_turns = 1,
        tools = {},
        update_status = false,
        update_usage = false,
    }
    if weak then
        opts.provider = weak.provider
        opts.model = weak.model
    end
    return opts, weak
end

_G.compact_entry = function(messages)
    if not messages or #messages == 0 then
        popup.error("Compact", "No conversation to compact")
        return
    end

    local chunks = {}
    local opts, weak = compact_run_options(messages)
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
                local message = result and result.error or "compact failed"
                popup.error("Compact", message)
                return
            end
            local text = result.text or table.concat(chunks)
            if text == "" then
                popup.error("Compact", "Compact returned an empty summary")
                return
            end
            agent.replace_compacted_context(text)
        end,
    })
end

-- Entry point called from C via agent_build_and_dispatch. Receives message
-- history as a Lua table, runs the full agent cycle with UI-visible streaming.
_G.agent_entry = function(messages)
    M.run({
        messages = messages,
        update_status = true,
        update_usage = true,
    }, {
        on_text = function(chunk)
            agent.append(chunk, "agent")
        end,
        on_error = function(message)
            popup.error("Provider", message)
        end,
    })
end

return M
