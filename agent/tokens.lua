local json = require("vendor.rxi.json")

local M = {}

function M.estimate_text_tokens(text)
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

function M.estimate_messages_tokens(messages, tools)
    local total = 0
    for _, message in ipairs(messages or {}) do
        total = total + 4
        total = total + M.estimate_text_tokens(message.role)
        total = total + M.estimate_text_tokens(message.content)
        if message.tool_calls then
            total = total + M.estimate_text_tokens(json.encode(message.tool_calls))
        end
    end
    if tools and #tools > 0 then
        total = total + M.estimate_text_tokens(json.encode(tools))
    end
    return total
end

return M
