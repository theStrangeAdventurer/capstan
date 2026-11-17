-- Pure policy: callers own discovery, transport, and persistence.
local M = {}
local NULL = {}
M.null = NULL
local kinds = setmetatable({}, { __mode = "k" })
local function finite(n)
    return type(n) == "number" and n == n and n ~= math.huge and n ~= -math.huge
end

-- Bounds cover auth responses/JWKS as well as the much smaller JWT payloads.
function M.decode_json(text)
    if type(text) ~= "string" or #text > 1048576 then return nil, "invalid JSON" end
    local function decode()
        local i, nodes = 1, 0
        local function fail() error("invalid JSON", 0) end
        local function ws()
            while text:sub(i, i):match("[ \t\r\n]") do i = i + 1 end
        end
        local function hex()
            local s = text:sub(i, i + 3)
            if #s ~= 4 or not s:match("^%x%x%x%x$") then fail() end
            i = i + 4
            return tonumber(s, 16)
        end
        local function str()
            if text:sub(i, i) ~= '"' then fail() end
            i = i + 1
            local out = {}
            while i <= #text do
                local c = text:sub(i, i)
                i = i + 1
                if c == '"' then
                    local s = table.concat(out)
                    if not utf8.len(s) then fail() end
                    return s
                elseif c == '\\' then
                    c = text:sub(i, i)
                    i = i + 1
                    local escapes = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/',
                        b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }
                    if c == 'u' then
                        local cp = hex()
                        if cp >= 0xD800 and cp <= 0xDBFF then
                            if text:sub(i, i + 1) ~= '\\u' then fail() end
                            i = i + 2
                            local low = hex()
                            if low < 0xDC00 or low > 0xDFFF then fail() end
                            cp = 0x10000 + (cp - 0xD800) * 1024 + low - 0xDC00
                        elseif cp >= 0xDC00 and cp <= 0xDFFF then fail() end
                        out[#out + 1] = utf8.char(cp)
                    elseif escapes[c] then out[#out + 1] = escapes[c]
                    else fail() end
                elseif c:byte() < 32 then fail()
                else out[#out + 1] = c end
            end
            fail()
        end
        local value
        value = function(depth)
            nodes = nodes + 1
            if depth > 32 or nodes > 65536 then fail() end
            ws()
            local c = text:sub(i, i)
            if c == '"' then return str() end
            if c == '{' or c == '[' then
                local object, result, seen = c == '{', {}, {}
                kinds[result] = object and "object" or "array"
                local close = object and '}' or ']'
                i = i + 1
                ws()
                if text:sub(i, i) == close then i = i + 1; return result end
                while true do
                    ws()
                    local key
                    if object then
                        key = str()
                        if seen[key] then fail() end
                        seen[key] = true
                        ws()
                        if text:sub(i, i) ~= ':' then fail() end
                        i = i + 1
                    else key = #result + 1 end
                    result[key] = value(depth + 1)
                    ws()
                    c = text:sub(i, i)
                    i = i + 1
                    if c == close then return result end
                    if c ~= ',' then fail() end
                end
            end
            for literal, v in pairs({ ['true'] = true, ['false'] = false, ['null'] = NULL }) do
                if text:sub(i, i + #literal - 1) == literal then i = i + #literal; return v end
            end
            local start = i
            if text:sub(i, i) == '-' then i = i + 1 end
            c = text:sub(i, i)
            if c == '0' then i = i + 1
            elseif c:match('[1-9]') then
                repeat i = i + 1 until not text:sub(i, i):match('%d')
            else fail() end
            if text:sub(i, i) == '.' then
                i = i + 1
                if not text:sub(i, i):match('%d') then fail() end
                repeat i = i + 1 until not text:sub(i, i):match('%d')
            end
            if text:sub(i, i):match('[eE]') then
                i = i + 1
                if text:sub(i, i):match('[+-]') then i = i + 1 end
                if not text:sub(i, i):match('%d') then fail() end
                repeat i = i + 1 until not text:sub(i, i):match('%d')
            end
            local n = tonumber(text:sub(start, i - 1))
            if not finite(n) then fail() end
            return n
        end
        local result = value(0)
        ws()
        if i <= #text then fail() end
        return result
    end
    local ok, result = pcall(decode)
    if not ok then return nil, "invalid JSON" end
    return result
end

local function object(t)
    return type(t) == "table" and t ~= NULL and kinds[t] ~= "array"
end
local function array(t)
    if type(t) ~= "table" or t == NULL or kinds[t] == "object" then return false end
    local count = 0
    for k in pairs(t) do
        if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then return false end
        count = count + 1
    end
    return count == #t
end
local function nonempty(s) return type(s) == "string" and #s > 0 end
local function b64(s)
    if not nonempty(s) or not s:match('^[A-Za-z0-9_-]+$') or #s % 4 == 1 then return false end
    -- Canonical unused bits; native decoders need not enforce these.
    local alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
    local last = alphabet:find(s:sub(-1), 1, true) - 1
    return (#s % 4 ~= 2 or last % 16 == 0) and (#s % 4 ~= 3 or last % 4 == 0)
end

-- expected: issuer, audience, now? (Unix seconds), clock_skew? (0..300,
-- default 60), nonce (required at sign-in), subject (required on refresh).
-- On refresh a missing nonce is allowed; supplying nonce always checks it.
function M.validate(token, jwks, expected)
    local function check()
        local function reject() error("invalid ID token", 0) end
        if type(token) ~= "string" or #token > 16384 or not object(expected) then reject() end
        if expected.issuer ~= "https://auth.openai.com" or not nonempty(expected.audience) then reject() end
        local now = expected.now
        if now == nil then now = os.time() end
        local skew = expected.clock_skew
        if skew == nil then skew = 60 end
        if not finite(now) or not finite(skew) or skew < 0 or skew > 300 then reject() end
        if expected.subject ~= nil and not nonempty(expected.subject) then reject() end
        if expected.nonce ~= nil and not nonempty(expected.nonce) then reject() end
        if expected.subject == nil and expected.nonce == nil then reject() end
        local h, p, s = token:match('^([^.]+)%.([^.]+)%.([^.]+)$')
        if not b64(h) or not b64(p) or not b64(s) then reject() end
        local crypto = capstan.oauth
        local header = M.decode_json(crypto.base64url_decode(h))
        local claims = M.decode_json(crypto.base64url_decode(p))
        if not object(header) or not object(claims) then reject() end
        if header.alg ~= "RS256" or not nonempty(header.kid) then reject() end
        for _, field in ipairs({ "crit", "jku", "x5u", "jwk", "x5c", "b64" }) do
            if header[field] ~= nil then reject() end
        end
        if type(jwks) == "string" then jwks = M.decode_json(jwks) end
        if not object(jwks) or not array(jwks.keys) then reject() end
        local key
        for _, candidate in ipairs(jwks.keys) do
            if not object(candidate) then reject() end
            if candidate.kid == header.kid then
                if key then reject() end
                key = candidate
            end
        end
        if not key or key.kty ~= "RSA" or (key.alg ~= nil and key.alg ~= "RS256")
            or (key.use ~= nil and key.use ~= "sig") or not b64(key.n) or not b64(key.e) then reject() end
        for _, field in ipairs({ "jku", "x5u", "jwk" }) do
            if key[field] ~= nil then reject() end
        end
        if key.key_ops ~= nil then
            if not array(key.key_ops) then reject() end
            local seen = {}
            for _, op in ipairs(key.key_ops) do
                if not nonempty(op) or seen[op] then reject() end
                seen[op] = true
            end
            if not seen.verify then reject() end
        end
        -- Signature and JWK integers stay base64url-encoded at the native boundary.
        if crypto.verify_rs256(h .. '.' .. p, s, key.n, key.e) ~= true then reject() end
        if claims.iss ~= expected.issuer or not nonempty(claims.sub) then reject() end
        if expected.subject and claims.sub ~= expected.subject then reject() end
        if expected.nonce and claims.nonce ~= expected.nonce then reject() end
        local aud, count, matched = claims.aud, 1, false
        if type(aud) == "string" then matched = aud == expected.audience
        elseif array(aud) then
            count = #aud
            local seen = {}
            for _, a in ipairs(aud) do
                if not nonempty(a) or seen[a] then reject() end
                seen[a] = true
                if a == expected.audience then matched = true end
            end
        else reject() end
        if not matched or (count > 1 and claims.azp ~= expected.audience)
            or (claims.azp ~= nil and claims.azp ~= expected.audience) then reject() end
        if not finite(claims.exp) or not finite(claims.iat)
            or claims.exp <= now - skew or claims.iat > now + skew
            or claims.exp <= claims.iat then reject() end
        if claims.nbf ~= nil and (not finite(claims.nbf) or claims.nbf > now + skew
            or claims.nbf >= claims.exp) then reject() end
        return claims
    end
    local ok, result = pcall(check)
    if not ok then return nil, "invalid ID token" end
    return result
end
return M
