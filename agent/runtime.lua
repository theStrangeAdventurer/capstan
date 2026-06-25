local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local models = require("agent.models")
local provider_config = require("agent.provider_config")
local stream = require("agent.stream")
local tokens = require("agent.tokens")
local tools_runtime = require("agent.tools")

local M = provider_config.build()

M.default_on_chunk = stream.default_on_chunk
hooks.install_config((_G.capstan and _G.capstan.config) or {})
hooks.install_existing_plugins(_G.plugins)

function M.list_models(provider_name)
    return models.list(M, provider_name or M.provider)
end

function M.set_model(provider_name, model)
    return models.set(M, provider_name or M.provider, model)
end

function M.stream(provider_name, on_result, initial_prompt_tokens)
    local provider = M.providers[provider_name]
    return stream.stream(provider, on_result, initial_prompt_tokens)
end

models.install_runtime_api(M)

local function build_messages(messages)
    local msgs = {}
    if _G.system_prompt and _G.system_prompt ~= "" then
        table.insert(msgs, {role = "system", content = _G.system_prompt})
    end
    for _, m in ipairs(messages or {}) do
        table.insert(msgs, {role = m.role, content = m.content})
    end
    return msgs
end

local function provider_for(opts)
    opts = opts or {}
    local provider_name = opts.provider or M.provider
    local active = M.providers[provider_name]
    if not active then
        return nil, provider_name
    end
    if opts.model and opts.model ~= "" then
        local copy = {}
        for k, v in pairs(active) do copy[k] = v end
        copy.model = opts.model
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

function M.run(opts, callbacks)
    opts = opts or {}
    callbacks = callbacks or {}
    local active, provider_name = provider_for(opts)
    if not active then
        local message = "Unknown provider: " .. tostring(provider_name)
        logging.runtime_log("provider", "unknown provider: " .. tostring(provider_name))
        if callbacks.on_error then callbacks.on_error(message) end
        if callbacks.on_done then callbacks.on_done({ok = false, error = message, text = ""}) end
        return false, message
    end

    if opts.update_status ~= false then
        agent.set_info(provider_name, active.model)
    end
    models.ensure_context_limit(active)
    if opts.update_usage ~= false then
        agent.set_usage(0, 0, 0, active.context_limit or 0)
    end

    local msgs = build_messages(opts.messages or {})
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
    logging.runtime_log("agent", string.format("request provider=%s model=%s messages=%d tools=%d",
        provider_name,
        active.model or "",
        #msgs,
        #combined_tools
    ))
    logging.runtime_log("agent", "tools=" .. tools_runtime.names(combined_tools))
    if #msgs > 0 then
        logging.runtime_log("agent", string.format("last_message role=%s content=%s",
            tostring(msgs[#msgs].role),
            logging.compact(msgs[#msgs].content, 300)
        ))
    end

    local max_turns = tonumber(opts.max_turns) or 20
    if max_turns <= 0 then max_turns = 20 end
    local turns = 0
    local started_at = capstan and capstan.now_ms and capstan.now_ms() or (os.clock() * 1000)

    local function finish(result)
        result = result or {}
        if result.ok == nil then result.ok = true end
        result.turns = result.turns or turns
        result.started_at = result.started_at or started_at
        result.finished_at = result.finished_at or (capstan and capstan.now_ms and capstan.now_ms() or (os.clock() * 1000))
        result.duration_ms = result.duration_ms or math.max(0, math.floor(result.finished_at - started_at))
        if callbacks.on_done then callbacks.on_done(result) end
    end

    local function continue(current_msgs, tools)
        turns = turns + 1
        if turns > max_turns then
            local message = "Max agent turns exceeded: " .. tostring(max_turns)
            logging.runtime_log("agent", message)
            if callbacks.on_error then callbacks.on_error(message) end
            finish({ok = false, error = message, text = ""})
            return
        end

        local function on_result(result, is_done)
            if not is_done then
                if result.type == "text" and result.content then
                    if callbacks.on_text then
                        callbacks.on_text(result.content)
                    else
                        agent.append(result.content, "agent")
                    end
                end
                return
            end

            if result.ok == false then
                local message = result.error or "agent stream failed"
                logging.runtime_log("agent", "stream failed error=" .. logging.compact(message, 500))
                if callbacks.on_error then callbacks.on_error(message) end
                finish({ok = false, error = message, text = result.text or ""})
                return
            end

            if result.tool_calls and #result.tool_calls > 0 then
                tools_runtime.process(current_msgs, tools, result.tool_calls, result.text, continue, {
                    runtime = M,
                    provider = active,
                    provider_name = provider_name,
                    depth = tonumber(opts.depth) or 0,
                    max_turns = max_turns,
                    tools = tools,
                    silent_tools = opts.silent_tools,
                    callbacks = callbacks,
                })
            else
                logging.runtime_log("agent", "stream done without tool calls text=" .. logging.compact(result.text, 500))
                hooks.run("after_agent_turn", {
                    runtime = M,
                    provider = active,
                    provider_name = provider_name,
                    messages = current_msgs,
                    tools = tools,
                    text = result.text or "",
                    run = opts,
                })
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
        local body = json.encode(request)

        logging.runtime_log("api", string.format("post_stream endpoint=%s messages=%d tools=%d",
            endpoint or "",
            #current_msgs,
            #tools
        ))

        http.post_stream(endpoint, body, headers,
            stream.stream(active, on_result, prompt_estimate))
    end

    continue(msgs, combined_tools)
    return true, nil
end

if not _G.capstan then _G.capstan = {} end
_G.capstan.agent = {
    run = function(opts, callbacks)
        return M.run(opts, callbacks)
    end,
}

_G.on_messages = function(messages)
    M.run({
        messages = messages,
        max_turns = 20,
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
