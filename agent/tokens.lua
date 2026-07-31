local json = require("vendor.rxi.json")

local M = {}

function M.estimate_text_tokens(text)
    if type(text) ~= "string" or text == "" then return 0 end
    -- Keep the common ASCII approximation while charging more for multibyte
    -- scripts: roughly two Cyrillic-style code points or one CJK/emoji code
    -- point per token. This remains tokenizer-independent but avoids the old
    -- four-Unicode-characters-per-token underestimate.
    local ascii_bytes = 0
    local two_byte_points = 0
    local wide_points = 0
    for i = 1, #text do
        local b = text:byte(i)
        if b < 128 then
            ascii_bytes = ascii_bytes + 1
        elseif b >= 194 and b <= 223 then
            two_byte_points = two_byte_points + 1
        elseif b >= 224 then
            wide_points = wide_points + 1
        end
    end
    local estimate = ascii_bytes / 4 + two_byte_points / 2 + wide_points
    return math.max(1, math.floor(estimate + 0.5))
end

local function estimate_content_tokens(content)
    if type(content) == "string" then return M.estimate_text_tokens(content) end
    if type(content) ~= "table" then return 0 end
    local total = 0
    for _, block in ipairs(content) do
        if type(block) == "table" then
            if block.type == "text" then
                total = total + M.estimate_text_tokens(block.text)
            elseif block.type == "image_url" then
                -- Image tokenization is model-specific. Use a bounded planning
                -- estimate and never count or log the base64 payload as text.
                total = total + 1024
            end
        end
    end
    return total
end

function M.estimate_messages_tokens(messages, tools)
    local total = 0
    for _, message in ipairs(messages or {}) do
        total = total + 4
        total = total + M.estimate_text_tokens(message.role)
        total = total + estimate_content_tokens(message.content)
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
