local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")
local tokens = require("agent.tokens")

local M = {}

local function structured_error_message(value)
    if type(value) == "string" and value ~= "" then return value end
    if type(value) ~= "table" then return nil end
    local message = type(value.message) == "string" and value.message or nil
    local code = type(value.code) == "string" and value.code or nil
    if message and code and not message:find(code, 1, true) then
        return code .. ": " .. message
    end
    return message or code
end

local function error_detail_from_body(body)
    if type(body) ~= "string" or body == "" then return nil end

    local function extract(value)
        if type(value) ~= "table" then return nil end
        return structured_error_message(value.error)
            or (type(value.response) == "table" and structured_error_message(value.response.error))
    end

    local ok, decoded = pcall(json.decode, body)
    if ok then
        local detail = extract(decoded)
        if detail then return detail end
    end

    for data in body:gmatch("data:%s*([^\r\n]+)") do
        local event_ok, event = pcall(json.decode, data)
        if event_ok then
            local detail = extract(event)
            if detail then return detail end
        end
    end
    local plain = body:gsub("<[^>]+>", " "):gsub("%s+", " ")
    plain = plain:match("^%s*(.-)%s*$") or ""
    return plain ~= "" and plain or nil
end

local function error_header_context(headers)
    if type(headers) ~= "table" then return nil, nil end
    local request_id = headers["x-request-id"]
        or headers["request-id"]
        or headers["openai-request-id"]
    local edge_id = headers["cf-ray"]
    if type(request_id) ~= "string" or request_id == "" then request_id = nil end
    if type(edge_id) ~= "string" or edge_id == "" then edge_id = nil end
    return request_id, edge_id
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

local function minimax_model(provider)
    local model = tostring(provider and provider.model or ""):lower()
    return model:find("minimax", 1, true) ~= nil
end

local function minimax_text_tool_protocol_error(provider, text)
    if not minimax_model(provider) or type(text) ~= "string" then return nil end

    local has_sentinel = text:find("]%<%]minimax%[%>%[", 1, false) ~= nil
        or text:find("</tool_call>", 1, true) ~= nil
        or text:find("</invoke>", 1, true) ~= nil
    local tool_name = text:match("^%s*⚙%s*([%w_%-]+)")
        or text:match("\n%s*⚙%s*([%w_%-]+)")
    local has_tool_fields = text:find("\n%s*path:%s*", 1, false) ~= nil
        or text:find("\n%s*old_text:%s*", 1, false) ~= nil
        or text:find("\n%s*new_text:%s*", 1, false) ~= nil
        or text:find("\n%s*command:%s*", 1, false) ~= nil

    if has_sentinel or (tool_name and has_tool_fields) then
        return "model emitted a textual tool call instead of structured tool_calls; " ..
            "switch models/providers or retry with a model that supports reliable tool calling"
    end
    return nil
end

local think_tags = {"<think>", "</think>"}

local function longest_tag_prefix_suffix(text, tags)
    local best = 0
    for _, tag in ipairs(tags) do
        local max_len = math.min(#tag - 1, #text)
        for len = 1, max_len do
            if text:sub(#text - len + 1) == tag:sub(1, len) and len > best then
                best = len
            end
        end
    end
    return best
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
        return {
            type = "reasoning",
            content = delta.reasoning_content,
            reasoning_details = delta.reasoning_details,
        }
    end
    if delta.reasoning and delta.reasoning ~= "" then
        return {
            type = "reasoning",
            content = delta.reasoning,
            reasoning_details = delta.reasoning_details,
        }
    end
    if type(delta.reasoning_details) == "table" then
        return {type = "reasoning", content = "", reasoning_details = delta.reasoning_details}
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
    local accumulated_reasoning_details = {}
    local reasoning_active = false
    local leaked_think_pending = ""
    local leaked_think_active = false
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
    local buffer_text_until_done = minimax_model(provider)
    local finished = false
    local provider_error = nil

    local function filter_leaked_think(delta, final)
        local s = leaked_think_pending .. (delta or "")
        leaked_think_pending = ""

        local out = {}
        local i = 1
        while i <= #s do
            if leaked_think_active then
                local close_start, close_end = s:find("</think>", i, true)
                if close_start then
                    leaked_think_active = false
                    i = close_end + 1
                else
                    if not final then
                        local rest = s:sub(i)
                        local hold = longest_tag_prefix_suffix(rest, {"</think>"})
                        if hold > 0 then
                            leaked_think_pending = rest:sub(#rest - hold + 1)
                        end
                    end
                    break
                end
            else
                local open_start, open_end = s:find("<think>", i, true)
                local close_start, close_end = s:find("</think>", i, true)
                local tag_start, tag_end, starts_hidden
                if open_start and (not close_start or open_start < close_start) then
                    tag_start, tag_end, starts_hidden = open_start, open_end, true
                elseif close_start then
                    tag_start, tag_end, starts_hidden = close_start, close_end, false
                end

                if tag_start then
                    if tag_start > i then
                        table.insert(out, s:sub(i, tag_start - 1))
                    end
                    leaked_think_active = starts_hidden
                    i = tag_end + 1
                else
                    local rest = s:sub(i)
                    if final then
                        table.insert(out, rest)
                    else
                        local hold = longest_tag_prefix_suffix(rest, think_tags)
                        if hold > 0 then
                            table.insert(out, rest:sub(1, #rest - hold))
                            leaked_think_pending = rest:sub(#rest - hold + 1)
                        else
                            table.insert(out, rest)
                        end
                    end
                    break
                end
            end
        end

        return table.concat(out)
    end

    local function process_chunk(chunk)
        if chunk.type == "provider_error" then
            provider_error = provider_error or logging.safe_error(chunk.error, 500)
            logging.runtime_log("stream", "provider_error=" .. provider_error, "error")
        elseif chunk.type == "reasoning" then
            reasoning_chunks = reasoning_chunks + 1
            local reasoning_content = chunk.content or ""
            accumulated_reasoning = accumulated_reasoning .. reasoning_content
            reasoning_token_estimate = reasoning_token_estimate + tokens.estimate_text_tokens(reasoning_content)
            for _, detail in ipairs(chunk.reasoning_details or {}) do
                table.insert(accumulated_reasoning_details, detail)
            end
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
            local content = filter_leaked_think(chunk.content, false)
            if content == "" then
                return
            end
            if reasoning_active then
                reasoning_active = false
                if not provider.suppress_agent_state then agent.set_thinking(false) end
            end
            accumulated_text = accumulated_text .. content
            text_token_estimate = text_token_estimate + tokens.estimate_text_tokens(content)
            if not provider.suppress_agent_state and provider.context_limit and provider.context_limit > 0 then
                local completion_estimate = text_token_estimate + reasoning_token_estimate
                agent.set_usage(
                    prompt_estimate,
                    completion_estimate,
                    prompt_estimate + completion_estimate,
                    provider.context_limit
                )
            end
            if buffer_text_until_done then
                return
            end
            on_result({type = "text", content = content}, false)
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
                logging.debug("stream", string.format(
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

    return function(raw, is_done, err, body, response_headers)
        if finished then return end
        if err then
            finished = true
            if reasoning_active and not provider.suppress_agent_state then agent.set_thinking(false) end
            local msg = logging.safe_error(err, 240)
            local detail = error_detail_from_body(body)
            local request_id, edge_id = error_header_context(response_headers)
            if detail then
                detail = logging.safe_error(detail, 500)
                logging.runtime_log("stream", "transport_error status=" .. msg ..
                    " detail=" .. detail, "error")
                if not msg:find(detail, 1, true) then
                    msg = logging.safe_error(msg .. ": " .. detail, 500)
                end
            else
                local unavailable = "provider returned an empty error body"
                logging.runtime_log("stream", "transport_error status=" .. msg ..
                    " body_detail=unavailable", "error")
                msg = logging.safe_error(msg .. ": " .. unavailable, 500)
            end
            if request_id then
                request_id = logging.safe_error(request_id, 160)
                logging.runtime_log("stream", "transport_request_id=" .. request_id, "error")
                msg = logging.safe_error(msg .. " (request id: " .. request_id .. ")", 700)
            end
            if edge_id then
                edge_id = logging.safe_error(edge_id, 160)
                logging.runtime_log("stream", "transport_edge_id=" .. edge_id, "error")
                msg = logging.safe_error(msg .. " (cf-ray: " .. edge_id .. ")", 800)
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
            local remaining_text = filter_leaked_think("", true)
            if remaining_text ~= "" then
                accumulated_text = accumulated_text .. remaining_text
                text_token_estimate = text_token_estimate + tokens.estimate_text_tokens(remaining_text)
                if not buffer_text_until_done then
                    on_result({type = "text", content = remaining_text}, false)
                end
            end

            local final_calls = {}
            local incomplete_calls = 0
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
                    incomplete_calls = incomplete_calls + 1
                    logging.runtime_log("stream", string.format(
                        "tool_incomplete id=%s name=%s args_bytes=%d",
                        logging.compact(tc.id, 80),
                        logging.compact(tc.name, 80),
                        #(tc.arguments or "")
                    ), "warn")
                end
            end

            if tool_delta_chunks > 0 and #final_calls == 0 then
                logging.runtime_log("stream", "tool_deltas_without_final_calls", "warn")
            end
            if accumulated_text ~= "" and #final_calls > 0 then
                logging.runtime_log("stream", string.format(
                    "mixed_text_and_tool_calls text_bytes=%d final_tool_calls=%d",
                    #accumulated_text,
                    #final_calls
                ), "warn")
            end
            if accumulated_text ~= "" and incomplete_calls > 0 then
                logging.runtime_log("stream", string.format(
                    "text_with_dropped_tool_calls text_bytes=%d incomplete_tool_calls=%d final_tool_calls=%d",
                    #accumulated_text,
                    incomplete_calls,
                    #final_calls
                ), "warn")
            end
            if provider_error then
                logging.runtime_log("stream", string.format(
                    "failed events=%d raw_bytes=%d text_bytes=%d reasoning_bytes=%d error=%s",
                    event_count,
                    raw_bytes,
                    #accumulated_text,
                    #accumulated_reasoning,
                    logging.compact(provider_error, 500)
                ), "error")
                on_result({
                    ok = false,
                    error = provider_error,
                    text = accumulated_text,
                    reasoning = accumulated_reasoning ~= "" and accumulated_reasoning or nil,
                    reasoning_details = #accumulated_reasoning_details > 0 and
                        accumulated_reasoning_details or nil,
                }, true)
                return
            end
            if accumulated_text == "" and #final_calls == 0 then
                local kind = accumulated_reasoning ~= "" and "reasoning_only_response" or "empty_response"
                logging.runtime_log("stream", string.format(
                    "%s events=%d raw_bytes=%d incomplete_tool_calls=%d",
                    kind,
                    event_count,
                    raw_bytes,
                    incomplete_calls
                ), "warn")
            end
            local protocol_error = minimax_text_tool_protocol_error(provider, accumulated_text)
            if #final_calls == 0 and protocol_error then
                logging.runtime_log("stream", "minimax_text_tool_call_protocol_error text=" ..
                    logging.compact(accumulated_text, 500))
                on_result({type = "text", content = "\n[provider error: " .. protocol_error .. "]\n"}, false)
                on_result({ok = false, error = protocol_error, text = ""}, true)
                return
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

            if buffer_text_until_done and #final_calls == 0 and accumulated_text ~= "" then
                on_result({type = "text", content = accumulated_text}, false)
            end
            on_result({
                text = accumulated_text,
                tool_calls = #final_calls > 0 and final_calls or nil,
                reasoning = accumulated_reasoning ~= "" and accumulated_reasoning or nil,
                reasoning_details = #accumulated_reasoning_details > 0 and
                    accumulated_reasoning_details or nil,
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
