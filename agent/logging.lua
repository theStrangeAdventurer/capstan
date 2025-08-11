local M = {}
local redact = require("agent.redact")
local utf8_sanitize = require("agent.utf8")

local function utf8_prefix(value, limit)
    if #value <= limit then return value end
    local cut = math.max(0, limit)
    while cut > 0 do
        local next_byte = value:byte(cut + 1)
        if not next_byte or next_byte < 0x80 or next_byte >= 0xC0 then break end
        cut = cut - 1
    end
    return value:sub(1, cut)
end

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
    return normalize_level(os.getenv("CAPSTAN_LOG_LEVEL")) or "info"
end

function M.enabled(level)
    local requested = normalize_level(level) or "info"
    return levels[requested] <= levels[M.level()]
end

function M.runtime_log(category, message, level)
    if not M.enabled(level or "info") then return end
    if _G.capstan and _G.capstan.log then
        local sanitized = utf8_sanitize.sanitize(redact.text(message or ""))
        _G.capstan.log(category, sanitized)
    end
end

function M.debug(category, message)
    M.runtime_log(category, message, "debug")
end

function M.trace(category, message)
    M.runtime_log(category, message, "trace")
end

function M.compact(value, limit)
    local s = utf8_sanitize.sanitize(redact.text(tostring(value or "")))
    s = s:gsub("%s+", " ")
    limit = limit or 240
    if #s > limit then
        return utf8_prefix(s, limit) .. "...<truncated>"
    end
    return s
end

function M.raw_logging_enabled()
    return M.enabled("trace")
end

function M.truncate(value, limit, suffix)
    local s = tostring(value or "")
    limit = math.max(0, math.floor(tonumber(limit) or 0))
    if #s <= limit then return s, false end
    suffix = tostring(suffix or "...<truncated>")
    local prefix_limit = math.max(0, limit - #suffix)
    return utf8_prefix(s, prefix_limit) .. suffix, true
end

function M.safe_error(value, limit)
    local s = utf8_sanitize.sanitize(redact.text(tostring(value or "error")))
    s = s:match("^[^\r\n]*") or ""
    s = s:gsub("%s+", " "):match("^%s*(.-)%s*$")
    if s == "" then s = "error" end
    return M.truncate(s, limit or 240)
end

return M
