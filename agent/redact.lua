local M = {}

local sensitive_headers = {
    authorization = true,
    ["proxy-authorization"] = true,
    cookie = true,
    ["set-cookie"] = true,
}

local env_fragments = {
    "TOKEN", "SECRET", "PASSWORD", "PASS", "AUTH",
}

local function config_redaction()
    local config = _G.capstan and type(_G.capstan.config) == "table" and _G.capstan.config or nil
    local redaction = config and config.redaction
    if type(redaction) ~= "table" then return {} end
    return redaction
end

local function array_contains_case_insensitive(values, candidate)
    if type(values) ~= "table" then return false end
    local lower = tostring(candidate or ""):lower()
    for _, value in ipairs(values) do
        if type(value) == "string" and value:lower() == lower then
            return true
        end
    end
    return false
end

local function any_lua_pattern_matches(patterns, value)
    if type(patterns) ~= "table" then return false end
    local s = tostring(value or "")
    for _, pattern in ipairs(patterns) do
        if type(pattern) == "string" and pattern ~= "" then
            local ok, matched = pcall(string.find, s, pattern)
            if ok and matched then return true end
        end
    end
    return false
end

local function apply_value_patterns(text)
    local result = tostring(text or "")
    local redaction = config_redaction()
    local patterns = redaction.value_patterns or redaction.extra_value_patterns
    if type(patterns) ~= "table" then return result end
    for _, pattern in ipairs(patterns) do
        if type(pattern) == "string" and pattern ~= "" then
            local ok, replaced = pcall(string.gsub, result, pattern, "[REDACTED]")
            if ok then result = replaced end
        end
    end
    return result
end

local function split_name(name)
    local parts = {}
    for part in tostring(name or ""):lower():gmatch("[a-z0-9]+") do
        table.insert(parts, part)
    end
    return parts
end

local function has_sensitive_key_context(name)
    local parts = split_name(name)
    for i, part in ipairs(parts) do
        if part == "apikey" then return true end
        if part == "key" then
            local prev = parts[i - 1]
            if prev == "api" or prev == "access" or prev == "secret" or
               prev == "private" or prev == "subscription" or
               prev == "openai" or prev == "anthropic" or prev == "goog" or
               prev == "google" then
                return true
            end
        end
    end
    return false
end

local function sensitive_name(name)
    local lower = tostring(name or ""):lower()
    local redaction = config_redaction()
    if array_contains_case_insensitive(redaction.names or redaction.extra_names, lower) then return true end
    if any_lua_pattern_matches(redaction.name_patterns or redaction.extra_name_patterns, lower) then return true end
    if sensitive_headers[lower] or lower == "passwd" then return true end
    if lower:find("token", 1, true) or lower:find("secret", 1, true) or
       lower:find("password", 1, true) or lower:find("auth", 1, true) then
        return true
    end
    return has_sensitive_key_context(lower)
end

local function redact_sensitive_key_values(text)
    local result = tostring(text or "")
    for _, fragment in ipairs(env_fragments) do
        result = result:gsub("(%f[%w_][A-Z0-9_]*" .. fragment .. "[A-Z0-9_]*%s*=%s*)[^%s,;}]+", "%1[REDACTED]")
    end
    result = result:gsub("([\"']?[%w%._%-]+[\"']?%s*[:=]%s*[\"']?)([^\"'%s,;}]+)", function(prefix, _value)
        local name = prefix:match("[\"']?([%w%._%-]+)[\"']?%s*[:=]")
        if sensitive_name(name) then
            return prefix .. "[REDACTED]"
        end
        return prefix .. _value
    end)
    return result
end

local function redact_header_line(line)
    local curl_prefix, name = line:match("^(%s*[<>]%s*)([%w%._%-]+)%s*:")
    if curl_prefix and name then
        if sensitive_name(name) then
            return line:gsub("(:%s*).*$", "%1[REDACTED]")
        end
        return line
    end
    name = line:match("^%s*([%w%._%-]+)%s*:")
    if name and sensitive_name(name) then
        return line:gsub("(:%s*).*$", "%1[REDACTED]")
    end
    return line
end

function M.text(text)
    if type(text) ~= "string" or text == "" then return text or "" end
    local result = apply_value_patterns(text)
    result = result:gsub("([Aa][Uu][Tt][Hh][Oo][Rr][Ii][Zz][Aa][Tt][Ii][Oo][Nn]%s*:%s*[Bb][Ee][Aa][Rr][Ee][Rr]%s+)[^%s\"']+", "%1[REDACTED]")
    result = result:gsub("([Aa][Uu][Tt][Hh][Oo][Rr][Ii][Zz][Aa][Tt][Ii][Oo][Nn]%s*:%s*)[^\r\n\"']+", "%1[REDACTED]")
    result = redact_sensitive_key_values(result)

    local lines = {}
    local had_line = false
    for line, newline in result:gmatch("([^\r\n]*)(\r?\n?)") do
        if line == "" and newline == "" then break end
        had_line = true
        table.insert(lines, redact_header_line(line) .. newline)
    end
    if had_line then result = table.concat(lines) end
    return result
end

return M
