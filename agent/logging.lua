local M = {}
local redact = require("agent.redact")

local levels = {
    error = 1,
    warn = 2,
    warning = 2,
    info = 3,
    debug = 4,
    trace = 5,
}

local function normalize_level(value)
    if type(value) ~= "string" then return nil end
    local normalized = value:lower():gsub("^%s+", ""):gsub("%s+$", "")
    if normalized == "" then return nil end
    return levels[normalized] and normalized or nil
end

function M.level()
    return normalize_level(os.getenv("LOG_LEVEL")) or "info"
end

function M.enabled(level)
    local requested = normalize_level(level) or "info"
    return levels[requested] <= levels[M.level()]
end

function M.runtime_log(category, message, level)
    if not M.enabled(level or "info") then return end
    if _G.capstan and _G.capstan.log then
        _G.capstan.log(category, redact.text(message or ""))
    end
end

function M.debug(category, message)
    M.runtime_log(category, message, "debug")
end

function M.trace(category, message)
    M.runtime_log(category, message, "trace")
end

function M.compact(value, limit)
    local s = redact.text(tostring(value or ""))
    s = s:gsub("%s+", " ")
    limit = limit or 240
    if #s > limit then
        return s:sub(1, limit) .. "...<truncated>"
    end
    return s
end

function M.raw_logging_enabled()
    return M.enabled("trace")
end

return M
