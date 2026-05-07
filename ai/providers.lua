local json = require("vendor.rxi.json")

local M = {}

M.provider = "deepseek"

M.providers = {
    deepseek = {
        api_key = os.getenv("DEEPSEEK_API_KEY"),
        endpoint = "https://api.deepseek.com/v1/chat/completions",
        model = "deepseek-chat",
    },
    openai = {
        api_key = os.getenv("OPENAI_API_KEY"),
        endpoint = "https://api.openai.com/v1/chat/completions",
        model = "gpt-4o",
    },
}

M.default_on_chunk = function(raw_event)
    local data = raw_event:match("^data: (.*)")
    if not data or data == "[DONE]" then return nil end
    local ok, event = pcall(json.decode, data)
    return ok and event.choices and event.choices[1].delta.content
end

M.stream = function(provider_name, on_text)
    local buf = ""
    local prov = M.providers[provider_name]
    local on_chunk = prov.on_chunk or M.default_on_chunk

    return function(raw, is_done)
        if is_done then
            if #buf > 0 then
                local text = on_chunk(buf)
                if text then on_text(text, false) end
            end
            on_text(nil, true)
            return
        end
        buf = buf .. raw
        while true do
            local sep = buf:find("\n\n", 1, true)
            if not sep then break end
            local event = buf:sub(1, sep - 1)
            buf = buf:sub(sep + 2)
            local text = on_chunk(event)
            if text then on_text(text, false) end
        end
    end
end

return M
