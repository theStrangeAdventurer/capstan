local M = {}

function M.runtime_log(category, message)
    if _G.capstan and _G.capstan.log then
        _G.capstan.log(category, message or "")
    end
end

function M.compact(value, limit)
    local s = tostring(value or "")
    s = s:gsub("%s+", " ")
    limit = limit or 240
    if #s > limit then
        return s:sub(1, limit) .. "...<truncated>"
    end
    return s
end

function M.raw_logging_enabled()
    local v = os.getenv("CAPSTAN_LOG_RAW")
    return v == "1" or v == "true" or v == "yes"
end

return M
