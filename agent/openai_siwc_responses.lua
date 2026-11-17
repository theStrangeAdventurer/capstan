-- SIWC HTTP Responses adapter; authentication belongs to openai_siwc_auth.
-- Schema: developers.openai.com/api/reference/resources/responses/methods/create/index.md
-- Route constraints: /siwc/token-sharing-open-source/preview-limitations.md
local json = require('vendor.rxi.json')
local M = {history_format = 'openai_siwc.responses.v1'}
local function check(ok, message) if not ok then error(message, 0) end end
local function string_value(v) return type(v) == 'string' and v ~= '' end
local function copy(v)
    if type(v) ~= 'table' then return v end
    local out = {}; for k, x in pairs(v) do out[k] = copy(x) end; return out
end
local function array(v)
    if type(v) ~= 'table' then return false end
    local n = 0; for k in pairs(v) do
        if type(k) ~= 'number' or k < 1 or k % 1 ~= 0 then return false end
        n = n + 1
    end
    return n == #v
end
local function arguments(v)
    check(type(v) == 'string' and v:match('^%s*{'), 'Function arguments must be a JSON object')
    local ok, decoded = pcall(json.decode, v)
    check(ok and type(decoded) == 'table', 'Invalid function arguments JSON')
    return v
end
local function equal(a, b)
    if type(a) ~= type(b) then return false end
    if type(a) ~= 'table' then return a == b end
    for k, v in pairs(a) do if not equal(v, b[k]) then return false end end
    for k in pairs(b) do if a[k] == nil then return false end end
    return true
end
local function reasoning(item)
    check(string_value(item.id) and array(item.summary), 'Invalid native reasoning item')
    local out = {type = 'reasoning', id = item.id, summary = {}}
    for _, part in ipairs(item.summary) do
        check(part.type == 'summary_text' and type(part.text) == 'string', 'Invalid reasoning summary')
        out.summary[#out.summary + 1] = {type = 'summary_text', text = part.text}
    end
    if item.encrypted_content ~= nil then
        check(type(item.encrypted_content) == 'string', 'Invalid encrypted reasoning')
        out.encrypted_content = item.encrypted_content
    end
    if item.content ~= nil then
        check(array(item.content), 'Invalid reasoning content')
        out.content = {}
        for _, part in ipairs(item.content) do
            check(part.type == 'reasoning_text' and type(part.text) == 'string', 'Invalid reasoning content')
            out.content[#out.content + 1] = {type = 'reasoning_text', text = part.text}
        end
    end
    check(item.status == nil or item.status == 'completed', 'Incomplete native reasoning')
    out.status = item.status
    return out
end
local function native_item(item)
    check(type(item) == 'table', 'Invalid native output item')
    if item.type == 'reasoning' then return reasoning(item) end
    check(item.status == nil or item.status == 'completed', 'Incomplete native output item')
    if item.type == 'function_call' then
        check(string_value(item.call_id) and string_value(item.name) and item.namespace == 'capstan',
            'Invalid function call identity/namespace')
        check(item.caller == nil or item.caller.type == 'direct', 'Programmatic calls are unsupported')
        check(not item.async, 'Async calls are unsupported')
        return {type = 'function_call', id = item.id, call_id = item.call_id,
            namespace = item.namespace, name = item.name, arguments = arguments(item.arguments), status = item.status}
    end
    check(item.type == 'message' and item.role == 'assistant' and array(item.content),
        'Unsupported native output item')
    check(item.phase == nil or item.phase == 'commentary' or item.phase == 'final_answer',
        'Invalid assistant phase')
    local out = {type = 'message', role = 'assistant', id = item.id, status = item.status,
        phase = item.phase, content = {}}
    for _, part in ipairs(item.content) do
        if part.type == 'output_text' then
            check(type(part.text) == 'string', 'Invalid output text')
            out.content[#out.content + 1] = {type = 'output_text', text = part.text, annotations = copy(part.annotations or {})}
        else
            check(part.type == 'refusal' and type(part.refusal) == 'string', 'Unsupported output content')
            out.content[#out.content + 1] = {type = 'refusal', refusal = part.refusal}
        end
    end
    return out
end

-- Returns body, request-local registry. Throws before dispatch on unsupported input.
-- messages is FULL ordered history, not just this turn. Native assistant messages
-- use responses=result.native_output (including its tool_names registry); do not
-- also replay their ordinary content/tool_calls. history.lua preserves this
-- field; runtime stamps and validates the local account binding.
function M.build_request(opts)
    check(type(opts) == 'table', 'Expected request options')
    for k in pairs(opts) do
        check(k == 'model' or k == 'messages' or k == 'tools' or k == 'reasoning',
            'Unsupported SIWC request option: ' .. tostring(k))
    end
    check(string_value(opts.model) and array(opts.messages), 'Model and full messages array required')
    local body = {model = opts.model, store = false, stream = true, input = {},
        include = {'reasoning.encrypted_content'}}
    if opts.reasoning ~= nil then
        check(type(opts.reasoning) == 'table', 'Unsupported SIWC reasoning: expected object')
        local efforts = {none=true, minimal=true, low=true, medium=true, high=true, xhigh=true, max=true}
        for field, value in pairs(opts.reasoning) do
            check(field == 'effort', 'Unsupported SIWC reasoning field: ' .. tostring(field))
            check(type(value) == 'string' and efforts[value], 'Unsupported SIWC reasoning effort')
        end
        if next(opts.reasoning) then body.reasoning = copy(opts.reasoning) end
    end
    local aliases, reverse = {}, {}
    local function reserve(alias, name)
        check(type(alias) == 'string' and #alias <= 128 and alias:match('^[a-zA-Z0-9_-]+$')
            and string_value(name), 'Invalid historical tool alias')
        check((not aliases[alias] or aliases[alias] == name) and
            (not reverse[name] or reverse[name] == alias), 'Conflicting historical tool alias')
        aliases[alias], reverse[name] = name, alias
    end
    for _, message in ipairs(opts.messages) do
        if type(message) == 'table' and type(message.responses) == 'table' then
            check(message.role == 'assistant' and message.responses.format == M.history_format,
                'Foreign or invalid native history')
            check(type(message.responses.tool_names) == 'table', 'Native history requires tool_names registry')
            for alias, name in pairs(message.responses.tool_names) do reserve(alias, name) end
        end
    end
    local next_alias = 0
    local function wire_name(name)
        check(string_value(name), 'Tool name must be nonempty')
        if reverse[name] then return reverse[name] end
        repeat next_alias = next_alias + 1 until not aliases['f_' .. next_alias]
        local alias = 'f_' .. next_alias
        reserve(alias, name)
        return alias
    end
    -- History precedes current definitions, so changing tool order cannot rename old calls.
    local names = {}
    for _, message in ipairs(opts.messages) do
        for _, call in ipairs(type(message) == 'table' and message.tool_calls or {}) do
            if type(call['function']) == 'table' then wire_name(call['function'].name) end
        end
    end
    check(opts.tools == nil or array(opts.tools), 'Tools must be an array')
    for _, tool in ipairs(opts.tools or {}) do
        check(tool.type == 'function' and type(tool['function']) == 'table', 'Only local function tools are supported')
        check(string_value(tool['function'].name), 'Tool name must be nonempty')
        names[#names + 1] = tool['function'].name
    end
    table.sort(names)
    for _, name in ipairs(names) do wire_name(name) end
    local registry, definitions = {tool_names = aliases}, {}
    for _, tool in ipairs(opts.tools or {}) do
        check(tool.type == 'function' and type(tool['function']) == 'table', 'Only local function tools are supported')
        local f = tool['function']; local name = wire_name(f.name)
        check(not registry[name], 'Duplicate tool name')
        registry[name] = f.name
        check(f.parameters == nil or type(f.parameters) == 'table', 'Invalid tool schema')
        check(f.strict == nil or type(f.strict) == 'boolean', 'Invalid strict setting')
        check(f.description == nil or type(f.description) == 'string', 'Invalid tool description')
        definitions[#definitions + 1] = {type = 'function', name = name,
            description = f.description, parameters = copy(f.parameters), strict = f.strict}
    end
    if #definitions > 0 then
        body.tools = {{type = 'namespace', name = 'capstan', description = 'Capstan local tools', tools = definitions}}
    end
    local calls, outputs = {}, {}
    local function add(item)
        if item.type == 'function_call' then
            check(not calls[item.call_id], 'Duplicate historical call ID')
            calls[item.call_id] = item
        end
        body.input[#body.input + 1] = item
    end
    for _, message in ipairs(opts.messages) do
        check(type(message) == 'table', 'Invalid history message')
        if message.responses then
            check(message.role == 'assistant' and message.responses.format == M.history_format and
                array(message.responses.items), 'Foreign or invalid native history')
            for _, item in ipairs(message.responses.items) do
                local normalized = native_item(item)
                check(normalized.type ~= 'function_call' or aliases[normalized.name], 'Unknown historical tool alias')
                add(normalized)
            end
        elseif message.role == 'tool' then
            local call = calls[message.tool_call_id]
            check(call and not outputs[message.tool_call_id], 'Unmatched or duplicate tool output')
            check(type(message.content) == 'string', 'Only text tool outputs are supported')
            outputs[message.tool_call_id] = true
            add({type = 'function_call_output', call_id = message.tool_call_id, output = message.content})
        else
            local role = message.role == 'system' and 'developer' or message.role
            check(role == 'developer' or role == 'user' or role == 'assistant', 'Unsupported message role')
            check(message.content == nil or type(message.content) == 'string', 'Only text message content is supported')
            if message.content ~= nil and message.content ~= '' then
                add({role = role, content = message.content})
            end
            check(message.tool_calls == nil or (role == 'assistant' and array(message.tool_calls)), 'Invalid historical tools')
            for _, call in ipairs(message.tool_calls or {}) do
                local f = call['function']
                check(type(f) == 'table' and string_value(call.id), 'Invalid historical function call')
                add({type = 'function_call', call_id = call.id, namespace = 'capstan',
                    name = wire_name(f.name), arguments = arguments(f.arguments)})
            end
        end
    end
    for id in pairs(calls) do check(outputs[id], 'Missing historical tool output') end
    return body, registry
end

-- Same callback signature as agent.stream.stream; only final ok=true results may
-- dispatch tools or persist native_output. Text deltas before that are provisional.
function M.stream(provider, registry, on_result, initial_prompt_tokens, run_opts)
    local terminal, failure, code, native, final_calls
    local done_items, seen_calls, added_items, deltas, final_arguments = {}, {}, {}, {}, {}
    local call_ids = {}
    local function observe(item)
        check(string_value(item.id), 'Missing output item ID')
        local added = added_items[item.id]
        if added then
            check(added.type == item.type, 'Contradictory output item type')
            if item.type == 'function_call' then
                for _, key in ipairs({'call_id', 'name', 'namespace'}) do
                    check(added[key] == item[key], 'Contradictory tool identity')
                end
            end
        end
        check(not seen_calls[item.id] or item.type == 'function_call', 'Tool item changed type')
        if item.type == 'function_call' then
            check(string_value(item.call_id) and string_value(item.name) and item.namespace == 'capstan',
                'Invalid function call identity/namespace')
            check(not call_ids[item.call_id] or call_ids[item.call_id] == item.id, 'Duplicate tool call ID')
            call_ids[item.call_id] = item.id
            seen_calls[item.id] = true
        end
    end
    local function finish_arguments(id, value)
        arguments(value)
        local prefix = deltas[id] or ''
        check(value:sub(1, #prefix) == prefix, 'Contradictory tool argument prefix')
        check(not final_arguments[id] or final_arguments[id] == value, 'Contradictory final tool arguments')
        final_arguments[id] = value
    end
    local function fail(message, error_code)
        failure = failure or message; code = code or error_code
    end
    local function parse(raw)
        local parts = {}
        for line in (raw .. '\n'):gmatch('(.-)\n') do
            local data = line:gsub('\r$', ''):match('^data:%s?(.*)$')
            if data then parts[#parts + 1] = data end
        end
        if #parts == 0 or table.concat(parts, '\n') == '[DONE]' then return nil end
        local event = json.decode(table.concat(parts, '\n'))
        check(type(event) == 'table' and string_value(event.type), 'Malformed Responses event')
        local kind = event.type
        if kind == 'error' or kind == 'response.failed' or kind == 'response.incomplete' then
            local detail = event.response and (event.response.error or event.response.incomplete_details) or event
            detail = type(detail) == 'table' and detail or {}
            fail(kind .. ': ' .. tostring(detail.code or detail.reason or detail.message or 'unknown'), detail.code)
            terminal = kind
            return nil
        end
        if kind == 'response.completed' then
            check(not terminal, 'Duplicate or contradictory terminal event')
            terminal = kind
            local response = event.response
            check(type(response) == 'table' and response.status == 'completed' and array(response.output),
                'Malformed response.completed')
            native, final_calls = {}, {}
            local ids = {}
            for _, item in ipairs(response.output) do
                local normalized = native_item(item)
                observe(item)
                check(not ids[item.id], 'Duplicate output item ID')
                check(not done_items[item.id] or equal(done_items[item.id], normalized), 'Contradictory final item snapshot')
                native[#native + 1] = normalized
                ids[item.id] = true
                if item.type == 'function_call' then
                    finish_arguments(item.id, item.arguments)
                    check(type(registry[item.name]) == 'string' and not ids['call:' .. item.call_id], 'Unknown or duplicate tool call')
                    ids['call:' .. item.call_id] = true
                    final_calls[#final_calls + 1] = {id = item.call_id, name = registry[item.name], arguments = normalized.arguments}
                end
            end
            for id in pairs(seen_calls) do check(ids[id], 'Completed response omitted a tool item') end
            for id in pairs(done_items) do check(ids[id], 'Completed response omitted a done item') end
            local chunks = {}
            if type(response.usage) == 'table' then
                local u = response.usage
                chunks[1] = {type = 'usage', usage = {prompt_tokens = u.input_tokens,
                    completion_tokens = u.output_tokens, total_tokens = u.total_tokens,
                    prompt_tokens_details = u.input_tokens_details, completion_tokens_details = u.output_tokens_details}}
            end
            return {type = 'batch', chunks = chunks,
                response_metadata = {response_id = response.id, response_model = response.model}}
        end
        check(not terminal, 'Event after terminal response')
        if kind == 'response.output_text.delta' then
            check(type(event.delta) == 'string', 'Malformed text delta')
            return {type = 'text', content = event.delta}
        elseif kind == 'response.output_item.added' or kind == 'response.output_item.done' then
            check(type(event.item) == 'table' and string_value(event.item.id), 'Malformed output item event')
            local item = event.item
            observe(item)
            if kind == 'response.output_item.added' then
                check(not added_items[item.id] and not done_items[item.id], 'Duplicate or late added item')
                added_items[item.id] = copy(item)
                if item.type == 'function_call' then
                    check(type(item.arguments) == 'string' and not deltas[item.id] and not final_arguments[item.id],
                        'Malformed or late initial tool arguments')
                    deltas[item.id] = item.arguments
                end
            else
                local normalized = native_item(item)
                check(not done_items[item.id] or equal(done_items[item.id], normalized), 'Contradictory done item snapshot')
                if item.type == 'function_call' then finish_arguments(item.id, item.arguments) end
                done_items[item.id] = normalized
            end
        elseif kind == 'response.function_call_arguments.delta' or kind == 'response.function_call_arguments.done' then
            check(string_value(event.item_id), 'Malformed tool argument event')
            check(type(kind:match('%.delta$') and event.delta or event.arguments) == 'string', 'Malformed tool arguments')
            seen_calls[event.item_id] = true
            check(not added_items[event.item_id] or added_items[event.item_id].type == 'function_call',
                'Arguments for non-tool item')
            check(not done_items[event.item_id] or done_items[event.item_id].type == 'function_call',
                'Arguments for non-tool item')
            if kind == 'response.function_call_arguments.delta' then
                check(not final_arguments[event.item_id], 'Tool argument delta after final arguments')
                deltas[event.item_id] = (deltas[event.item_id] or '') .. event.delta
            else
                finish_arguments(event.item_id, event.arguments)
            end
        end
        return nil
    end
    local adapted = {}; for k, v in pairs(provider or {}) do adapted[k] = v end
    adapted.parse_sse_event = function(raw)
        local ok, chunk = pcall(parse, raw)
        if not ok then fail('Responses protocol error: ' .. tostring(chunk)) end
        -- Shared stream logging must never echo untrusted server error text.
        if failure then return {type = 'provider_error', error = terminal or 'Responses protocol error'} end
        return chunk
    end
    local callback = require('agent.stream').stream(adapted, function(result, done)
        if not done then on_result(result, false); return end
        if result.ok == false or failure or terminal ~= 'response.completed' then
            result.ok = false
            result.error = result.error or failure or 'Stream ended without response.completed'
            result.error_category = result.error_category or 'protocol'
            result.error_code = code
            result.tool_calls, result.native_output = nil, nil
            if result.metrics then result.metrics.request_outcome = 'incomplete' end
        else
            result.ok = true
            result.tool_calls = #final_calls > 0 and final_calls or nil
            result.native_output = {format = M.history_format, items = native, tool_names = copy(registry.tool_names)}
            if result.metrics then result.metrics.request_outcome = #final_calls > 0 and 'tool_calls' or 'completed' end
        end
        on_result(result, true)
    end, initial_prompt_tokens, run_opts)
    return function(raw, done, err, body, headers, transport)
        -- Shared framing ignores raw on EOF; this adapter accepts a final packet.
        if done and not err and type(raw) == 'string' and raw ~= '' then callback(raw, false) end
        if err then
            err = err == 'cancelled' and 'cancelled' or 'SIWC connection error'
            body, headers = nil, nil
        end
        callback(done and '' or raw, done, err, body, headers, transport)
    end
end
return M
