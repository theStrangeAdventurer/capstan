local M = {}

local profiles = {}
local registration_order = {}
local registration_sequence = 0
local default_name = "implement"

local function refresh_default()
    local selected, selected_order = "implement", -1
    for name, profile in pairs(profiles) do
        local order = registration_order[name] or 0
        if profile.default == true and order > selected_order then
            selected, selected_order = name, order
        end
    end
    default_name = selected
end

local function trimmed(value)
    if type(value) ~= "string" then return nil end
    value = value:lower():gsub("^%s+", ""):gsub("%s+$", "")
    if value == "" or not value:match("^[%w_-]+$") then return nil end
    return value
end

local function copy(value)
    if type(value) ~= "table" then return value end
    local out = {}
    for key, item in pairs(value) do out[key] = copy(item) end
    return out
end

local function valid_prompt(value)
    if type(value) == "string" then return true end
    if type(value) ~= "table" then return false end
    local count = 0
    for key, item in pairs(value) do
        count = count + 1
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 or
           key > #value or type(item) ~= "string" then return false end
    end
    return count == #value
end

local function validate(definition, source)
    if type(definition) ~= "table" then
        return nil, source .. " must return a profile table"
    end
    local name = trimmed(definition.name)
    if not name then return nil, source .. " has an invalid profile name" end
    if definition.allowed_tools ~= nil then
        if type(definition.allowed_tools) ~= "table" then
            return nil, source .. " allowed_tools must be a table"
        end
        for tool, allowed in pairs(definition.allowed_tools) do
            if type(tool) ~= "string" or tool == "" or type(allowed) ~= "boolean" then
                return nil, source .. " allowed_tools must map tool names to booleans"
            end
        end
    end
    if definition.prompt ~= nil and not valid_prompt(definition.prompt) then
        return nil, source .. " prompt must be a string or an array of strings"
    end
    if definition.prompt_append ~= nil and type(definition.prompt_append) ~= "string" then
        return nil, source .. " prompt_append must be a string"
    end
    return name
end

local function register(definition, source)
    local name, validation_err = validate(definition, source)
    if not name then return nil, validation_err end
    local current = profiles[name]
    local merged = definition.replace and {} or copy(current or {})
    for key, value in pairs(definition) do
        if key ~= "replace" and key ~= "prompt_append" then merged[key] = copy(value) end
    end
    merged.name = name
    if definition.prompt_append then
        if type(merged.prompt) == "table" then
            table.insert(merged.prompt, definition.prompt_append)
        elseif type(merged.prompt) == "string" and merged.prompt ~= "" then
            merged.prompt = merged.prompt .. "\n\n" .. definition.prompt_append
        else
            merged.prompt = definition.prompt_append
        end
    end
    merged._source = source
    profiles[name] = merged
    registration_sequence = registration_sequence + 1
    registration_order[name] = registration_sequence
    refresh_default()
    return merged
end

local function load_module(module)
    local ok, value = pcall(require, module)
    if not ok then error("failed to load " .. module .. ": " .. tostring(value)) end
    local registered, err = register(value, "embedded:" .. module:gsub("%.", "/") .. ".lua")
    if not registered then error(err) end
end

load_module("profiles.implement")
load_module("profiles.plan")

local runtime_options = _G.capstan and _G.capstan.runtime_options or {}
if not runtime_options.isolated and _G.capstan and type(_G.capstan.profile_files) == "function" then
    for _, path in ipairs(_G.capstan.profile_files()) do
        local chunk, load_err = loadfile(path)
        local ok, value = false, load_err
        if chunk then ok, value = pcall(chunk) end
        if ok then
            local _, register_err = register(value, path)
            if register_err then io.stderr:write("Error loading profile: " .. register_err .. "\n") end
        else
            io.stderr:write("Error loading profile " .. path .. ": " .. tostring(value) .. "\n")
        end
    end
end

local configured = _G.capstan and _G.capstan.config and _G.capstan.config.agent
if not runtime_options.isolated and type(configured) == "table" and type(configured.profiles) == "table" then
    for name, patch in pairs(configured.profiles) do
        if type(patch) == "table" then
            patch = copy(patch)
            patch.name = patch.name or name
            local _, err = register(patch, "config.lua")
            if err then io.stderr:write("Error loading profile: " .. err .. "\n") end
        end
    end
end

function M.normalize(value)
    local name = trimmed(value)
    return name and profiles[name] and name or nil
end

function M.get(value)
    local name = M.normalize(value)
    return name and profiles[name] or nil
end

function M.default_name()
    return profiles[default_name] and default_name or "implement"
end

function M.names()
    local out = {}
    for name in pairs(profiles) do table.insert(out, name) end
    table.sort(out, function(a, b)
        local left, right = profiles[a], profiles[b]
        local lo, ro = tonumber(left.order) or 1000, tonumber(right.order) or 1000
        if lo == ro then return a < b end
        return lo < ro
    end)
    return out
end

local function tool_name(tool)
    if type(tool) ~= "table" then return nil end
    local fn = tool["function"]
    return type(fn) == "table" and fn.name or nil
end

function M.filter_tools(tools, profile)
    if not profile or not profile.allowed_tools then return tools end
    local filtered = {}
    for _, tool in ipairs(tools or {}) do
        local name = tool_name(tool)
        if name and profile.allowed_tools[name] then table.insert(filtered, tool) end
    end
    return filtered
end

return M
