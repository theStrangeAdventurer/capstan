local M = {}

local REPLACEMENT = "\239\191\189"

local function continuation(byte)
    return byte and byte >= 0x80 and byte <= 0xBF
end

local function sequence_length(value, index)
    local b1 = value:byte(index)
    if not b1 then return 0 end
    if b1 <= 0x7F then return 1 end

    local b2 = value:byte(index + 1)
    if b1 >= 0xC2 and b1 <= 0xDF and continuation(b2) then
        return 2
    end

    local b3 = value:byte(index + 2)
    if b1 == 0xE0 and b2 and b2 >= 0xA0 and b2 <= 0xBF and continuation(b3) then
        return 3
    end
    if ((b1 >= 0xE1 and b1 <= 0xEC) or (b1 >= 0xEE and b1 <= 0xEF)) and
       continuation(b2) and continuation(b3) then
        return 3
    end
    if b1 == 0xED and b2 and b2 >= 0x80 and b2 <= 0x9F and continuation(b3) then
        return 3
    end

    local b4 = value:byte(index + 3)
    if b1 == 0xF0 and b2 and b2 >= 0x90 and b2 <= 0xBF and
       continuation(b3) and continuation(b4) then
        return 4
    end
    if b1 >= 0xF1 and b1 <= 0xF3 and
       continuation(b2) and continuation(b3) and continuation(b4) then
        return 4
    end
    if b1 == 0xF4 and b2 and b2 >= 0x80 and b2 <= 0x8F and
       continuation(b3) and continuation(b4) then
        return 4
    end
    return nil
end

function M.sanitize(value)
    if type(value) ~= "string" or value == "" then return value, 0 end
    local parts = {}
    local replacements = 0
    local index = 1
    local valid_start = 1

    while index <= #value do
        local length = sequence_length(value, index)
        if length then
            index = index + length
        else
            if index > valid_start then
                table.insert(parts, value:sub(valid_start, index - 1))
            end
            table.insert(parts, REPLACEMENT)
            replacements = replacements + 1
            index = index + 1
            valid_start = index
        end
    end

    if replacements == 0 then return value, 0 end
    if valid_start <= #value then
        table.insert(parts, value:sub(valid_start))
    end
    return table.concat(parts), replacements
end

function M.sanitize_values(value, seen)
    if type(value) == "string" then return M.sanitize(value) end
    if type(value) ~= "table" then return value, 0 end
    seen = seen or {}
    if seen[value] then return value, 0 end
    seen[value] = true

    local replacements = 0
    local key_changes = {}
    for key, item in pairs(value) do
        local sanitized, count = M.sanitize_values(item, seen)
        value[key] = sanitized
        replacements = replacements + count
        if type(key) == "string" then
            local sanitized_key, key_count = M.sanitize(key)
            replacements = replacements + key_count
            if sanitized_key ~= key then
                table.insert(key_changes, {old = key, new = sanitized_key})
            end
        end
    end
    for _, change in ipairs(key_changes) do
        if value[change.new] ~= nil then
            error("request contains colliding keys after invalid UTF-8 replacement")
        end
        value[change.new] = value[change.old]
        value[change.old] = nil
    end
    return value, replacements
end

return M
