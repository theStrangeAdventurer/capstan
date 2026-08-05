local M = {}

M.MAX_IMAGE_BYTES = 10 * 1024 * 1024

local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

function M.base64_encode(data)
    if type(data) ~= "string" or data == "" then return "" end
    local chunks = {}
    for chunk_start = 1, #data, 3072 do
        local chars = {}
        local chunk_end = math.min(#data, chunk_start + 3071)
        for i = chunk_start, chunk_end, 3 do
            local a = data:byte(i) or 0
            local b = data:byte(i + 1)
            local c = data:byte(i + 2)
            local value = a * 65536 + (b or 0) * 256 + (c or 0)
            local c1 = math.floor(value / 262144) % 64
            local c2 = math.floor(value / 4096) % 64
            local c3 = math.floor(value / 64) % 64
            local c4 = value % 64
            chars[#chars + 1] = alphabet:sub(c1 + 1, c1 + 1) ..
                alphabet:sub(c2 + 1, c2 + 1) ..
                (b and alphabet:sub(c3 + 1, c3 + 1) or "=") ..
                (c and alphabet:sub(c4 + 1, c4 + 1) or "=")
        end
        chunks[#chunks + 1] = table.concat(chars)
    end
    return table.concat(chunks)
end

function M.detect_mime(data)
    if type(data) ~= "string" then return nil end
    if data:sub(1, 8) == "\137PNG\r\n\26\n" then return "image/png" end
    if data:sub(1, 3) == "\255\216\255" then return "image/jpeg" end
    local gif = data:sub(1, 6)
    if gif == "GIF87a" or gif == "GIF89a" then return "image/gif" end
    if data:sub(1, 4) == "RIFF" and data:sub(9, 12) == "WEBP" then
        return "image/webp"
    end
    return nil
end

function M.from_bytes(data)
    if type(data) ~= "string" then return nil, "invalid" end
    local mime_type = M.detect_mime(data)
    if not mime_type then return nil, "unsupported" end
    if #data > M.MAX_IMAGE_BYTES then return nil, "too_large" end
    return {
        mime_type = mime_type,
        data = M.base64_encode(data),
        bytes = #data,
    }
end

local decode_values = {}
for i = 1, #alphabet do decode_values[alphabet:sub(i, i)] = i - 1 end

local function base64_decode_prefix(data, max_bytes)
    local chunks = {}
    local decoded = 0
    for i = 1, #data, 4 do
        local a = decode_values[data:sub(i, i)]
        local b = decode_values[data:sub(i + 1, i + 1)]
        local c_char = data:sub(i + 2, i + 2)
        local d_char = data:sub(i + 3, i + 3)
        local c = c_char == "=" and 0 or decode_values[c_char]
        local d = d_char == "=" and 0 or decode_values[d_char]
        local value = a * 262144 + b * 4096 + c * 64 + d
        local chunk = string.char(
            math.floor(value / 65536) % 256,
            math.floor(value / 256) % 256,
            value % 256
        ):sub(1, d_char == "=" and (c_char == "=" and 1 or 2) or 3)
        local remaining = max_bytes - decoded
        if #chunk > remaining then chunk = chunk:sub(1, remaining) end
        chunks[#chunks + 1] = chunk
        decoded = decoded + #chunk
        if decoded >= max_bytes then break end
    end
    return table.concat(chunks)
end

function M.from_mcp(item)
    if type(item) ~= "table" or type(item.data) ~= "string" then
        return nil, "invalid"
    end
    local mime_type = tostring(item.mimeType or item.mime_type or ""):lower()
    local data = item.data:gsub("%s", "")
    local padding = data:match("(=*)$") or ""
    local payload = data:sub(1, #data - #padding)
    if data == "" or #data % 4 ~= 0 or #padding > 2 or payload == "" or
        not payload:match("^[A-Za-z0-9+/]+$") then
        return nil, "invalid"
    end
    local decoded_bytes = math.floor(#data * 3 / 4) - #padding
    if decoded_bytes > M.MAX_IMAGE_BYTES then return nil, "too_large" end
    -- File signatures fit in the first 16 decoded bytes. Decode only that
    -- prefix so a valid 10 MiB image does not create millions of Lua strings.
    local decoded_prefix = base64_decode_prefix(data, 16)
    local detected_mime = M.detect_mime(decoded_prefix)
    if not detected_mime then return nil, "unsupported" end
    if mime_type ~= detected_mime then return nil, "mime_mismatch" end
    return {mime_type = detected_mime, data = data, bytes = decoded_bytes}
end

function M.is_text(data)
    if type(data) ~= "string" then return false end
    if data:find("\0", 1, true) then return false end
    return utf8.len(data) ~= nil
end

return M
