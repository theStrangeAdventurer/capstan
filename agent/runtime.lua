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

_G.on_messages = function(messages)
    local active = M.providers[M.provider]
    if not active then
        logging.runtime_log("provider", "unknown provider: " .. tostring(M.provider))
        popup.error("Provider", "Unknown provider: " .. M.provider)
        return
    end
    agent.set_info(M.provider, active.model)
    models.ensure_context_limit(active)
    agent.set_usage(0, 0, 0, active.context_limit or 0)

    local msgs = {}
    if _G.system_prompt and _G.system_prompt ~= "" then
        table.insert(msgs, {role = "system", content = _G.system_prompt})
    end
    for _, m in ipairs(messages) do
        table.insert(msgs, {role = m.role, content = m.content})
    end
    local messages_ctx = hooks.run("before_messages", {
        runtime = M,
        provider = active,
        provider_name = M.provider,
        messages = msgs,
    })
    msgs = messages_ctx.messages or msgs

    local combined_tools = tools_runtime.collect()
    local tools_ctx = hooks.run("before_tools", {
        runtime = M,
        provider = active,
        provider_name = M.provider,
        messages = msgs,
        tools = combined_tools,
    })
    combined_tools = tools_ctx.tools or combined_tools
    logging.runtime_log("agent", string.format("request provider=%s model=%s messages=%d tools=%d",
        M.provider,
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

    local function continue(current_msgs, tools)
        local function on_result(result, is_done)
            if not is_done then
                if result.type == "text" and result.content then
                    agent.append(result.content, "agent")
                end
                return
            end

            if result.tool_calls and #result.tool_calls > 0 then
                tools_runtime.process(current_msgs, tools, result.tool_calls, result.text, continue)
            else
                logging.runtime_log("agent", "stream done without tool calls text=" .. logging.compact(result.text, 500))
                hooks.run("after_agent_turn", {
                    runtime = M,
                    provider = active,
                    provider_name = M.provider,
                    messages = current_msgs,
                    tools = tools,
                    text = result.text or "",
                })
            end
        end

        local prompt_estimate = tokens.estimate_messages_tokens(current_msgs, tools)
        agent.set_usage(
            prompt_estimate,
            0,
            prompt_estimate,
            active.context_limit or 0
        )

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
            provider_name = M.provider,
            messages = current_msgs,
            tools = tools,
            request = request,
            headers = headers,
            endpoint = active.endpoint,
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
            M.stream(M.provider, on_result, prompt_estimate))
    end

    continue(msgs, combined_tools)
end

return M
