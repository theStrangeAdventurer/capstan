local json = require("vendor.rxi.json")

local M = {}

M.provider = os.getenv("AI_PROVIDER") or "deepseek"

local function runtime_log(category, message)
    if _G.capstan and _G.capstan.log then
        _G.capstan.log(category, message or "")
    end
end

local function compact(value, limit)
    local s = tostring(value or "")
    s = s:gsub("%s+", " ")
    limit = limit or 240
    if #s > limit then
        return s:sub(1, limit) .. "...<truncated>"
    end
    return s
end

local function raw_logging_enabled()
    local v = os.getenv("CAPSTAN_LOG_RAW")
    return v == "1" or v == "true" or v == "yes"
end

local function env_context_limit(...)
    for i = 1, select("#", ...) do
        local value = os.getenv(select(i, ...))
        if value and value ~= "" then
            local n = tonumber(value)
            if n and n > 0 then return n end
        end
    end
    return 0
end

M.providers = {
    deepseek = {
        api_key = os.getenv("DEEPSEEK_API_KEY"),
        endpoint = "https://api.deepseek.com/v1/chat/completions",
        model = "deepseek-chat",
        context_limit = env_context_limit("DEEPSEEK_CONTEXT_LIMIT", "AI_CONTEXT_LIMIT"),
    },
    openrouter = {
        api_key = os.getenv("OPENROUTER_API_KEY"),
        endpoint = "https://openrouter.ai/api/v1/chat/completions",
        model = os.getenv("OPENROUTER_MODEL") or "minimax/minimax-m3",
        context_limit = env_context_limit("OPENROUTER_CONTEXT_LIMIT", "AI_CONTEXT_LIMIT"),
    },
}

local openrouter_models_cache = nil
local openrouter_models_loaded = false

local model_aliases = {
    ["deepseek-chat"] = {
        "deepseek/deepseek-chat",
        "deepseek/deepseek-v3",
        "deepseek/deepseek-v3.2",
        "deepseek/deepseek-v3.1",
    },
}

local function model_candidates(model)
    local candidates = {model}
    local aliases = model_aliases[model]
    if aliases then
        for _, alias in ipairs(aliases) do
            table.insert(candidates, alias)
        end
    end
    return candidates
end

local function model_context_length(item)
    local top = item.top_provider and item.top_provider.context_length or 0
    if top and top > 0 then return top end
    if item.context_length and item.context_length > 0 then
        return item.context_length
    end
    return 0
end

local function normalized_model_id(value)
    if not value then return "" end
    return tostring(value):lower():gsub("[^a-z0-9]+", "")
end

local function fuzzy_model_match(item, model)
    local id = tostring(item.id or ""):lower()
    local name = tostring(item.name or ""):lower()
    local haystack = id .. " " .. name

    if model == "deepseek-chat" then
        return haystack:find("deepseek", 1, true) and
               (haystack:find("chat", 1, true) or haystack:find("v3", 1, true))
    end

    return false
end

local function openrouter_model_limit(model)
    if not model or model == "" then return 0 end

    if not openrouter_models_loaded then
        openrouter_models_loaded = true
        local headers = {}
        local key = os.getenv("OPENROUTER_API_KEY")
        if key and key ~= "" then
            headers["Authorization"] = "Bearer " .. key
        end

        local status, body = http.get("https://openrouter.ai/api/v1/models", headers)
        if status == 200 and body and body ~= "" then
            local ok, decoded = pcall(json.decode, body)
            if ok and decoded and decoded.data then
                openrouter_models_cache = decoded.data
            end
        end
    end

    if not openrouter_models_cache then return 0 end

    local candidates = model_candidates(model)
    for _, item in ipairs(openrouter_models_cache) do
        for _, candidate in ipairs(candidates) do
            if item.id == candidate or item.canonical_slug == candidate then
                return model_context_length(item)
            end
            local wanted = normalized_model_id(candidate)
            if wanted ~= "" and
               (normalized_model_id(item.id) == wanted or
                normalized_model_id(item.canonical_slug) == wanted) then
                return model_context_length(item)
            end
        end
    end

    for _, item in ipairs(openrouter_models_cache) do
        if fuzzy_model_match(item, model) then
            local limit = model_context_length(item)
            if limit > 0 then return limit end
        end
    end

    return 0
end

local function ensure_context_limit(provider)
    if provider.context_limit and provider.context_limit > 0 then
        return provider.context_limit
    end
    provider.context_limit = openrouter_model_limit(provider.model)
    return provider.context_limit or 0
end

local function estimate_text_tokens(text)
    if not text or text == "" then return 0 end
    local chars = 0
    for i = 1, #text do
        local b = text:byte(i)
        if b < 128 or b >= 192 then
            chars = chars + 1
        end
    end
    return math.max(1, math.floor(chars / 4 + 0.5))
end

local function estimate_messages_tokens(messages, tools)
    local total = 0
    for _, message in ipairs(messages or {}) do
        total = total + 4
        total = total + estimate_text_tokens(message.role)
        total = total + estimate_text_tokens(message.content)
        if message.tool_calls then
            total = total + estimate_text_tokens(json.encode(message.tool_calls))
        end
    end
    if tools and #tools > 0 then
        total = total + estimate_text_tokens(json.encode(tools))
    end
    return total
end

M.default_on_chunk = function(raw_event)
    local data = raw_event:match("^data: (.*)")
    if not data or data == "[DONE]" then return nil end
    local ok, event = pcall(json.decode, data)
    if not ok then
        runtime_log("stream", "invalid_json event=" .. compact(raw_event))
        return nil
    end
    if event.usage then
        return {type = "usage", usage = event.usage}
    end
    if not event.choices or not event.choices[1] then return nil end
    local finish_reason = event.choices[1].finish_reason
    local delta = event.choices[1].delta or {}
    if finish_reason then
        runtime_log("stream", "finish_reason=" .. tostring(finish_reason))
    end
    if delta.reasoning_content and delta.reasoning_content ~= "" then
        return {type = "reasoning", content = delta.reasoning_content}
    end
    if delta.reasoning and delta.reasoning ~= "" then
        return {type = "reasoning", content = delta.reasoning}
    end
    if delta.tool_calls then
        return {type = "tool_calls", tool_calls = delta.tool_calls}
    end
    if delta.content and delta.content ~= "" then
        return {type = "text", content = delta.content}
    end
    return nil
end

M.stream = function(provider_name, on_result, initial_prompt_tokens)
    local buf = ""
    local accumulated_text = ""
    local accumulated_reasoning = ""
    local reasoning_active = false
    local tool_calls_accum = {}
    local prov = M.providers[provider_name]
    local on_chunk = prov.on_chunk or M.default_on_chunk
    local prompt_estimate = initial_prompt_tokens or 0
    local event_count = 0
    local raw_bytes = 0
    local text_chunks = 0
    local reasoning_chunks = 0
    local tool_delta_chunks = 0
    local usage_chunks = 0

    local function process_chunk(chunk)
        if chunk.type == "reasoning" then
            reasoning_chunks = reasoning_chunks + 1
            accumulated_reasoning = accumulated_reasoning .. chunk.content
            if prov.context_limit and prov.context_limit > 0 then
                local completion_estimate =
                    estimate_text_tokens(accumulated_text) +
                    estimate_text_tokens(accumulated_reasoning)
                agent.set_usage(
                    prompt_estimate,
                    completion_estimate,
                    prompt_estimate + completion_estimate,
                    prov.context_limit
                )
            end
            if not reasoning_active then
                reasoning_active = true
                agent.set_thinking(true)
            end
        elseif chunk.type == "text" and chunk.content then
            text_chunks = text_chunks + 1
            if reasoning_active then
                reasoning_active = false
                agent.set_thinking(false)
            end
            accumulated_text = accumulated_text .. chunk.content
            if prov.context_limit and prov.context_limit > 0 then
                local completion_estimate =
                    estimate_text_tokens(accumulated_text) +
                    estimate_text_tokens(accumulated_reasoning)
                agent.set_usage(
                    prompt_estimate,
                    completion_estimate,
                    prompt_estimate + completion_estimate,
                    prov.context_limit
                )
            end
            on_result({type = "text", content = chunk.content}, false)
        elseif chunk.type == "tool_calls" then
            tool_delta_chunks = tool_delta_chunks + 1
            for _, tc in ipairs(chunk.tool_calls) do
                local idx = tc.index or 0
                if not tool_calls_accum[idx] then
                    tool_calls_accum[idx] = {id = "", name = "", arguments = ""}
                end
                if tc.id then tool_calls_accum[idx].id = tc.id end
                if tc["function"] then
                    if tc["function"].name then
                        tool_calls_accum[idx].name = tool_calls_accum[idx].name .. tc["function"].name
                    end
                    if tc["function"].arguments then
                        tool_calls_accum[idx].arguments = tool_calls_accum[idx].arguments .. tc["function"].arguments
                    end
                end
                runtime_log("stream", string.format(
                    "tool_delta index=%d id=%s name=%s args_bytes=%d",
                    idx,
                    compact(tool_calls_accum[idx].id, 64),
                    compact(tool_calls_accum[idx].name, 80),
                    #tool_calls_accum[idx].arguments
                ))
            end
        elseif chunk.type == "usage" and chunk.usage then
            usage_chunks = usage_chunks + 1
            agent.set_usage(
                chunk.usage.prompt_tokens or 0,
                chunk.usage.completion_tokens or 0,
                chunk.usage.total_tokens or 0,
                prov.context_limit or 0
            )
        end
    end

    return function(raw, is_done, err, body)
        if err then
            if reasoning_active then agent.set_thinking(false) end
            local msg = err
            if body and body ~= "" then
                msg = err .. "\n" .. body
            end
            popup.error("API Error", msg)
            return
        end
        if is_done then
            if reasoning_active then
                reasoning_active = false
                agent.set_thinking(false)
            end
            if #buf > 0 then
                local chunk = on_chunk(buf)
                if chunk then process_chunk(chunk) end
            end

            local final_calls = {}
            for _, tc in pairs(tool_calls_accum) do
                if tc.id ~= "" and tc.name ~= "" and tc.arguments ~= "" then
                    tc.name = tc.name:match("^%s*(.-)%s*$")
                    table.insert(final_calls, tc)
                    runtime_log("stream", string.format(
                        "tool_final id=%s name=%s args=%s",
                        compact(tc.id, 80),
                        compact(tc.name, 80),
                        compact(tc.arguments, 260)
                    ))
                else
                    runtime_log("stream", string.format(
                        "tool_incomplete id=%s name=%s args_bytes=%d",
                        compact(tc.id, 80),
                        compact(tc.name, 80),
                        #(tc.arguments or "")
                    ))
                end
            end

            if tool_delta_chunks > 0 and #final_calls == 0 then
                runtime_log("stream", "tool_deltas_without_final_calls")
            end

            runtime_log("stream", string.format(
                "done events=%d raw_bytes=%d text_chunks=%d reasoning_chunks=%d tool_delta_chunks=%d usage_chunks=%d text_bytes=%d reasoning_bytes=%d final_tool_calls=%d",
                event_count,
                raw_bytes,
                text_chunks,
                reasoning_chunks,
                tool_delta_chunks,
                usage_chunks,
                #accumulated_text,
                #accumulated_reasoning,
                #final_calls
            ))

            on_result({
                text = accumulated_text,
                tool_calls = #final_calls > 0 and final_calls or nil
            }, true)
            return
        end

        raw_bytes = raw_bytes + #(raw or "")
        if raw_logging_enabled() then
            runtime_log("raw_sse", compact(raw, 1200))
        end

        buf = buf .. raw
        while true do
            local sep = buf:find("\n\n", 1, true)
            if not sep then break end
            local event = buf:sub(1, sep - 1)
            buf = buf:sub(sep + 2)
            event_count = event_count + 1
            if raw_logging_enabled() then
                runtime_log("sse_event", compact(event, 1200))
            end
            local chunk = on_chunk(event)
            if chunk then process_chunk(chunk) end
        end
    end
end

local function collect_tools()
    local tools = {}
    if not _G.plugins then return tools end
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
    return tools
end

local function tool_names(tools)
    local names = {}
    for _, tool in ipairs(tools or {}) do
        if tool["function"] and tool["function"].name then
            table.insert(names, tool["function"].name)
        end
    end
    table.sort(names)
    return table.concat(names, ",")
end

local function call_plugin_tool(tool_name, args)
    if not _G.plugins then return "No plugins loaded" end
    for _, p in pairs(_G.plugins) do
        if p.tool and p.tool.name == tool_name then
            local ctx_args = {}
            for _, v in pairs(args) do
                table.insert(ctx_args, tostring(v))
            end
            local ctx = {
                input = "/" .. tool_name,
                command = p.command,
                args = ctx_args,
                tool_args = args,
            }
            function ctx:replace(ui_val, llm_val)
                return ui_val, llm_val or ui_val
            end
            local ui_result, llm_result = p.handler(ctx)
            return llm_result or ui_result
        end
    end
    return "Unknown tool: " .. tool_name
end

local function tool_call_target(tool_name, args)
    return args.command or args.path or args.url or args.uri or tool_name
end

local function process_tool_calls(current_msgs, combined_tools, tool_calls, assistant_text, continue_fn)
    runtime_log("tools", string.format("received %d tool call(s)", #tool_calls))
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
        local args = {}
        local ok = pcall(json.decode, tc.arguments)
        if ok then args = json.decode(tc.arguments) end

        local target = tool_call_target(tc.name, args)
        runtime_log("tool", string.format("call name=%s target=%s args=%s", tc.name, target, tc.arguments or ""))
        local perm = permit.check(tc.name, target)
        runtime_log("permit", string.format("tool=%s target=%s decision=%s", tc.name, target, perm))

        agent.append(string.format("\n⚙ %s: %s ", tc.name, target), "agent")

        local result_content
        if perm == "deny" then
            result_content = "Permission denied for " .. tc.name .. " " .. target
            agent.append("— denied\n\n", "agent")
        elseif perm == "ask" then
            local decision = permit.prompt(tc.name, target)
            runtime_log("permit", string.format("tool=%s target=%s prompt=%s", tc.name, target, decision))
            if decision == "deny" then
                result_content = "User denied " .. tc.name .. " " .. target
                agent.append("— denied by user\n\n", "agent")
            else
                if decision == "always" then
                    permit.grant(tc.name, target, true)
                    permit.save()
                end
                result_content = call_plugin_tool(tc.name, args)
                runtime_log("tool", string.format("done name=%s target=%s bytes=%d", tc.name, target, #(result_content or "")))
                agent.append("— done\n\n", "agent")
            end
        else
            result_content = call_plugin_tool(tc.name, args)
            runtime_log("tool", string.format("done name=%s target=%s bytes=%d", tc.name, target, #(result_content or "")))
            agent.append("— done\n\n", "agent")
        end

        if result_content then
            table.insert(current_msgs, {
                role = "tool",
                tool_call_id = tc.id,
                content = result_content
            })
        end
    end

    runtime_log("tools", "continuing with tool results")
    continue_fn(current_msgs, combined_tools)
end

_G.on_messages = function(messages)
    local active = M.providers[M.provider]
    if not active then
        runtime_log("provider", "unknown provider: " .. tostring(M.provider))
        popup.error("Provider", "Unknown provider: " .. M.provider)
        return
    end
    agent.set_info(M.provider, active.model)
    ensure_context_limit(active)
    agent.set_usage(0, 0, 0, active.context_limit or 0)

    local msgs = {}
    if _G.system_prompt and _G.system_prompt ~= "" then
        table.insert(msgs, {role = "system", content = _G.system_prompt})
    end
    for _, m in ipairs(messages) do
        table.insert(msgs, {role = m.role, content = m.content})
    end

    local combined_tools = collect_tools()
    runtime_log("agent", string.format("request provider=%s model=%s messages=%d tools=%d",
        M.provider,
        active.model or "",
        #msgs,
        #combined_tools
    ))
    runtime_log("agent", "tools=" .. tool_names(combined_tools))
    if #msgs > 0 then
        runtime_log("agent", string.format("last_message role=%s content=%s",
            tostring(msgs[#msgs].role),
            compact(msgs[#msgs].content, 300)
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
                process_tool_calls(current_msgs, tools, result.tool_calls, result.text, continue)
            else
                runtime_log("agent", "stream done without tool calls text=" .. compact(result.text, 500))
            end
        end

        local prompt_estimate = estimate_messages_tokens(current_msgs, tools)
        agent.set_usage(
            prompt_estimate,
            0,
            prompt_estimate,
            active.context_limit or 0
        )

        local body = json.encode({
            model = active.model,
            messages = current_msgs,
            tools = #tools > 0 and tools or nil,
            tool_choice = #tools > 0 and "auto" or nil,
            stream = true,
            stream_options = {include_usage = true}
        })

        runtime_log("api", string.format("post_stream endpoint=%s messages=%d tools=%d",
            active.endpoint or "",
            #current_msgs,
            #tools
        ))
        http.post_stream(active.endpoint, body, {
            ["Content-Type"] = "application/json",
            ["Authorization"] = "Bearer " .. active.api_key
        }, M.stream(M.provider, on_result, prompt_estimate))
    end

    continue(msgs, combined_tools)
end

return M
