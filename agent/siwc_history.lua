-- Account binding is local metadata; never add credentials to native history.
local format = require('agent.openai_siwc_responses').history_format
local M = {}
local invalid = 'SIWC native history has an invalid format; start a new conversation'
local unbound = 'SIWC native history is not bound to the active account; start a new conversation'
local function account(value) return type(value) == 'string' and value ~= '' end
local function copy(value)
    if type(value) ~= 'table' then return value end
    local out = {}
    for key, item in pairs(value) do out[key] = copy(item) end
    return out
end
function M.stamp(native_output, account_id)
    if type(native_output) ~= 'table' or native_output.format ~= format then return nil, invalid end
    if not account(account_id) then return nil, unbound end
    local out = copy(native_output)
    out.account_id = account_id
    return out
end
function M.validate(messages, account_id)
    for _, message in ipairs(messages or {}) do
        local native = message.responses
        if native ~= nil then
            if message.role ~= 'assistant' or type(native) ~= 'table' or native.format ~= format then
                return nil, invalid
            end
            -- Pre-release unstamped records cannot safely be assigned to an account.
            if not account(account_id) or not account(native.account_id) or native.account_id ~= account_id then
                return nil, unbound
            end
        end
    end
    return true
end
return M
