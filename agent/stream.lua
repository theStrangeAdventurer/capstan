local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local tokens = require("agent.tokens")

local M = {}

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

local function summarize_shell_command(command)
    if type(command) ~= "string" or command == "" then return "shell" end
    local first = unquote_shell_token(command:match("^%s*([^%s]+)") or "")
    if first:match("/?curl$") or first == "curl" then
        for token in command:gmatch("%S+") do
            local clean_token = unquote_shell_token(token)
            if clean_token:match("^https?://") then
                return "curl " .. clean_token
            end
        end
        return "curl"
    end
    return "shell"
end

local function loggable_tool_arguments(name, raw_arguments)
    if name ~= "shell" then return raw_arguments end
    local ok, decoded = pcall(json.decode, raw_arguments or "{}")
    if ok and type(decoded) == "table" then
        local copy = {}
        for k, v in pairs(decoded) do copy[k] = v end
        copy.command = summarize_shell_command(decoded.command)
        local encoded_ok, encoded = pcall(json.encode, copy)
        if encoded_ok then return encoded end
    end
    return "{\"command\":\"shell\"}"
end

-- Parses one raw SSE event line into a typed chunk: text, reasoning, tool_calls, or usage.
function M.parse_sse_event(raw_event)
    local data = raw_event:match("^data: (.*)")
    if not data or data == "[DONE]" then return nil end
    local ok, event = pcall(json.decode, data)
    if not ok then
        logging.runtime_log("stream", "invalid_json event=" .. logging.compact(raw_event))
        return nil
    end
    if event.usage then
        return {type = "usage", usage = event.usage}
    end
    if not event.choices or not event.choices[1] then return nil end
    local finish_reason = event.choices[1].finish_reason
    local delta = event.choices[1].delta or {}
    if finish_reason then
        logging.runtime_log("stream", "finish_reason=" .. tostring(finish_reason))
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

-- Builds the raw→chunk→callback pipeline. Returns a closure suitable as the
-- callback argument to http.post_stream. Accumulates text, reasoning, and
-- tool_call fragments across partial SSE chunks, calls on_result for each
-- text delta and a final result with collected tool_calls on stream end.
function M.stream(provider, on_result, initial_prompt_tokens, run_opts)
    local buf = ""
    local accumulated_text = ""
    local accumulated_reasoning = ""
    local reasoning_active = false
    local tool_calls_accum = {}
    local parse_sse_event = provider.parse_sse_event or M.parse_sse_event
    local prompt_estimate = initial_prompt_tokens or 0
    local event_count = 0
    local raw_bytes = 0
    local text_chunks = 0
    local reasoning_chunks = 0
    local tool_delta_chunks = 0
    local usage_chunks = 0
    local text_token_estimate = 0
    local reasoning_token_estimate = 0
    local has_chunk_hooks = hooks.has("on_stream_chunk")
    local finished = false

    local function process_chunk(chunk)
        if chunk.type == "reasoning" then
            reasoning_chunks = reasoning_chunks + 1
            accumulated_reasoning = accumulated_reasoning .. chunk.content
            reasoning_token_estimate = reasoning_token_estimate + tokens.estimate_text_tokens(chunk.content)
            if not provider.suppress_agent_state and provider.context_limit and provider.context_limit > 0 then
                local completion_estimate = text_token_estimate + reasoning_token_estimate
                agent.set_usage(
                    prompt_estimate,
                    completion_estimate,
                    prompt_estimate + completion_estimate,
                    provider.context_limit
                )
            end
            if not reasoning_active then
                reasoning_active = true
                if not provider.suppress_agent_state then agent.set_thinking(true) end
            end
        elseif chunk.type == "text" and chunk.content then
            text_chunks = text_chunks + 1
            if reasoning_active then
                reasoning_active = false
                if not provider.suppress_agent_state then agent.set_thinking(false) end
            end
            accumulated_text = accumulated_text .. chunk.content
            text_token_estimate = text_token_estimate + tokens.estimate_text_tokens(chunk.content)
            if not provider.suppress_agent_state and provider.context_limit and provider.context_limit > 0 then
                local completion_estimate = text_token_estimate + reasoning_token_estimate
                agent.set_usage(
                    prompt_estimate,
                    completion_estimate,
                    prompt_estimate + completion_estimate,
                    provider.context_limit
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
                logging.runtime_log("stream", string.format(
                    "tool_delta index=%d id=%s name=%s args_bytes=%d",
                    idx,
                    logging.compact(tool_calls_accum[idx].id, 64),
                    logging.compact(tool_calls_accum[idx].name, 80),
                    #tool_calls_accum[idx].arguments
                ))
            end
        elseif chunk.type == "usage" and chunk.usage then
            usage_chunks = usage_chunks + 1
            if not provider.suppress_agent_state then
                agent.set_usage(
                    chunk.usage.prompt_tokens or 0,
                    chunk.usage.completion_tokens or 0,
                    chunk.usage.total_tokens or 0,
                    provider.context_limit or 0
                )
            end
        end
    end

    return function(raw, is_done, err, body)
        if finished then return end
        if err then
            finished = true
            if reasoning_active and not provider.suppress_agent_state then agent.set_thinking(false) end
            local msg = err
            if body and body ~= "" then
                msg = err .. "\n" .. body
            end
            if not provider.suppress_agent_state then
                popup.error("API Error", msg)
            end
            on_result({ok = false, error = msg, text = ""}, true)
            return
        end
        if is_done then
            finished = true
            if reasoning_active then
                reasoning_active = false
                if not provider.suppress_agent_state then agent.set_thinking(false) end
            end
            if #buf > 0 then
                local chunk = parse_sse_event(buf)
                if chunk and has_chunk_hooks then
                    local ctx = hooks.run("on_stream_chunk", {
                        provider = provider,
                        raw_event = buf,
                        chunk = chunk,
                        run = run_opts or {},
                    })
                    chunk = ctx.chunk
                end
                if chunk then process_chunk(chunk) end
            end

            local final_calls = {}
            for _, tc in pairs(tool_calls_accum) do
                if tc.id ~= "" and tc.name ~= "" and tc.arguments ~= "" then
                    tc.name = tc.name:match("^%s*(.-)%s*$")
                    table.insert(final_calls, tc)
                    logging.runtime_log("stream", string.format(
                        "tool_final id=%s name=%s args=%s",
                        logging.compact(tc.id, 80),
                        logging.compact(tc.name, 80),
                        logging.compact(loggable_tool_arguments(tc.name, tc.arguments), 260)
                    ))
                else
                    logging.runtime_log("stream", string.format(
                        "tool_incomplete id=%s name=%s args_bytes=%d",
                        logging.compact(tc.id, 80),
                        logging.compact(tc.name, 80),
                        #(tc.arguments or "")
                    ))
                end
            end

            if tool_delta_chunks > 0 and #final_calls == 0 then
                logging.runtime_log("stream", "tool_deltas_without_final_calls")
            end

            logging.runtime_log("stream", string.format(
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
        if logging.raw_logging_enabled() then
            logging.runtime_log("raw_sse", logging.compact(raw, 1200))
        end

        buf = buf .. raw
        while true do
            local sep = buf:find("\n\n", 1, true)
            if not sep then break end
            local event = buf:sub(1, sep - 1)
            buf = buf:sub(sep + 2)
            event_count = event_count + 1
            if logging.raw_logging_enabled() then
                logging.runtime_log("sse_event", logging.compact(event, 1200))
            end
            local chunk = parse_sse_event(event)
            if chunk and has_chunk_hooks then
                local ctx = hooks.run("on_stream_chunk", {
                        provider = provider,
                        raw_event = event,
                        chunk = chunk,
                        run = run_opts or {},
                    })
                chunk = ctx.chunk
            end
            if chunk then process_chunk(chunk) end
        end
    end
end

return M
