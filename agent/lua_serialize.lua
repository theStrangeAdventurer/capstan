local M = {}

-- Serializes a Lua string literal for state files.
function M.quote(value)
    local s = tostring(value or "")
    s = s:gsub("\\", "\\\\")
         :gsub("\n", "\\n")
         :gsub("\r", "\\r")
         :gsub("\t", "\\t")
         :gsub("\"", "\\\"")
    return "\"" .. s .. "\""
end

function M.write_value(writer, value, indent)
    indent = indent or ""
    if type(value) == "string" then
        writer:write(M.quote(value))
    elseif type(value) == "number" or type(value) == "boolean" then
        writer:write(tostring(value))
    elseif type(value) == "table" then
        writer:write("{\n")
        local keys = {}
        for k, _ in pairs(value) do
            if type(k) == "string" then table.insert(keys, k) end
        end
        table.sort(keys)
        for _, k in ipairs(keys) do
            writer:write(indent, "  [", M.quote(k), "] = ")
            M.write_value(writer, value[k], indent .. "  ")
            writer:write(",\n")
        end
        writer:write(indent, "}")
    else
        writer:write("nil")
    end
end

function M.encode_return(value)
    local chunks = {}
    local writer = {
        write = function(_, ...)
            for i = 1, select("#", ...) do
                table.insert(chunks, tostring(select(i, ...)))
            end
        end
    }
    writer:write("return ")
    M.write_value(writer, value, "")
    writer:write("\n")
    return table.concat(chunks)
end

return M
