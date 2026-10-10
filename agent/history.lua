-- Model history is independent of the terminal transcript. Each assistant UI
-- record may carry the exact structured messages produced by its agent run.
local json = require('vendor.rxi.json')
local M = {}
local function copy(message)
    if type(message) ~= 'table' or not ({user=true, assistant=true, tool=true, system=true, developer=true})[message.role] then
        error('Invalid persisted model history message')
    end
    local out = {}
    for _, key in ipairs({'role','content','tool_calls','tool_call_id','reasoning',
        'reasoning_content','reasoning_details','name'}) do out[key] = message[key] end
    return out
end
function M.expand(messages)
    local result = {}
    for _, message in ipairs(messages or {}) do
        if message.model_history_json then
            local ok, decoded = pcall(json.decode, message.model_history_json)
            if not ok or type(decoded) ~= 'table' or #decoded == 0 then
                error('Cannot restore structured model history')
            end
            for _, item in ipairs(decoded) do
                if type(item) ~= 'table' or item.role == 'system' or item.role == 'developer' then
                    error('Invalid instruction role in persisted turn')
                end
                result[#result + 1] = copy(item)
            end
        else result[#result + 1] = copy(message) end
    end
    return result
end
function M.encode(messages, initial, excluded, text)
    local result = {}
    for _, message in ipairs(messages or {}) do
        if not initial[message] and not excluded[message] then result[#result+1] = copy(message) end
    end
    if type(text) == 'string' and text ~= '' then
        result[#result+1] = {role='assistant', content=text}
    end
    return #result > 0 and json.encode(result) or nil
end
return M
