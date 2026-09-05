local logging = require("agent.logging")
local utf8_sanitize = require("agent.utf8")
local M = {}

local function limits()
    local capstan = _G.capstan
    local isolated = capstan and type(capstan.runtime_options) == "table" and
        capstan.runtime_options.isolated
    local config = not isolated and capstan and capstan.config
    local configured = type(config) == "table" and config.tool_output or nil
    if type(configured) ~= "table" then configured = {} end
    local max_bytes = tonumber(configured.max_bytes) or (50 * 1024)
    local max_lines = tonumber(configured.max_lines) or 2000
    return math.max(1024, math.floor(max_bytes)),
        math.max(1, math.floor(max_lines))
end

-- Shared by model results and shell UI/log output. Redact before bounding.
function M.bound(text)
    local sanitized, invalid_bytes = utf8_sanitize.sanitize(text)
    local max_bytes, max_lines = limits()
    local line_count = 1
    local line_cut = nil
    for newline in sanitized:gmatch("()\n") do
        if line_count == max_lines then
            line_cut = newline
            break
        end
        line_count = line_count + 1
    end
    if not line_cut and #sanitized <= max_bytes then
        return sanitized, false, invalid_bytes
    end

    local original_bytes = #sanitized
    local candidate = line_cut and sanitized:sub(1, line_cut - 1) or sanitized
    local suffix = string.format(
        "\n\n[Tool output truncated: %d bytes; use a narrower query or a paged read.]",
        original_bytes
    )
    local prefix = logging.truncate(candidate, math.max(0, max_bytes - #suffix), "")
    return prefix .. suffix, true, invalid_bytes
end

return M
