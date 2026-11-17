-- Run from repository root: vendor/lua-5.5.0/src/lua test/test_oidc.lua
local oidc = dofile('agent/oidc.lua')
local alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
local decoded = {}
local function encode(text)
    local out, bits, n = {}, 0, 0
    for i = 1, #text do
        bits, n = bits * 256 + text:byte(i), n + 8
        while n >= 6 do
            n = n - 6
            local v = math.floor(bits / 2 ^ n)
            out[#out + 1] = alphabet:sub(v + 1, v + 1)
            bits = bits % 2 ^ n
        end
    end
    if n > 0 then
        local v = bits * 2 ^ (6 - n)
        out[#out + 1] = alphabet:sub(v + 1, v + 1)
    end
    local result = table.concat(out)
    decoded[result] = text
    return result
end
local calls, signature_ok, last = 0, true
capstan = { oauth = {
    base64url_decode = function(s) return decoded[s] end,
    verify_rs256 = function(input, sig, n, e)
        calls = calls + 1
        last = { input, sig, n, e }
        return signature_ok
    end,
} }
local header = '{"alg":"RS256","kid":"key"}'
local claims = '{"iss":"https://auth.openai.com","aud":"client","sub":"user","iat":900,"exp":1100,"nonce":"fresh"}'
local function token(h, p) return encode(h or header) .. '.' .. encode(p or claims) .. '.c2ln' end
local function expected()
    return { issuer = 'https://auth.openai.com', audience = 'client', now = 1000,
        clock_skew = 0, nonce = 'fresh' }
end
local function keys()
    return { keys = { { kid = 'key', kty = 'RSA', n = 'AQAB', e = 'AQAB' } } }
end
local checks = 0
local function good(t, k, e)
    local result, err = oidc.validate(t or token(), k or keys(), e or expected())
    assert(result and result.sub == 'user' and err == nil)
    checks = checks + 1
end
local function bad(t, k, e)
    local result, err = oidc.validate(t or token(), k or keys(), e or expected())
    assert(result == nil and err == 'invalid ID token')
    checks = checks + 1
end
local function claim(old, new) return token(nil, (claims:gsub(old, new))) end

-- RFC 4648 URL-safe alphabet: sextets 62 and 63 are '-' and '_'.
assert(encode(string.char(251, 255, 255)) == '-___')
assert(encode(string.char(255, 239, 190)) == '_---')

good()
assert(calls == 1 and last[1] == encode(header) .. '.' .. encode(claims))
assert(last[2] == 'c2ln' and last[3] == 'AQAB' and last[4] == 'AQAB')
-- Full sextets may contain either URL-safe symbol; partial final sextets
-- cannot end in either symbol because their unused bits would be nonzero.
good((token():gsub('c2ln$', '-___')))
good((token():gsub('c2ln$', '_---')))
for _, signature in ipairs({ 'A-', 'A_', 'AA-', 'AA_' }) do
    bad((token():gsub('c2ln$', signature)))
end
signature_ok = false; bad(); signature_ok = true
local verify = capstan.oauth.verify_rs256
capstan.oauth.verify_rs256 = function() error('secret token') end
bad()
capstan.oauth.verify_rs256 = verify
for _, alg in ipairs({ 'none', 'HS256', 'RS512' }) do
    bad(token((header:gsub('RS256', alg))))
end
for _, field in ipairs({ 'crit', 'jku', 'x5u', 'jwk', 'x5c', 'b64' }) do
    bad(token((header:gsub('}$', ',"' .. field .. '":null}'))))
end
bad(token((header:gsub('"key"', '"unknown"'))))
local k = keys(); k.keys[2] = k.keys[1]; bad(nil, k)
for field, value in pairs({ kty = 'EC', alg = 'HS256', use = 'enc', n = 'bad=', e = '' }) do
    k = keys(); k.keys[1][field] = value; bad(nil, k)
end
for _, field in ipairs({ 'jku', 'x5u', 'jwk' }) do
    k = keys(); k.keys[1][field] = 'https://untrusted.invalid'; bad(nil, k)
end
for _, ops in ipairs({ {}, { 'sign' }, { 'verify', 'verify' }, { false } }) do
    k = keys(); k.keys[1].key_ops = ops; bad(nil, k)
end
k = keys(); k.keys[1].key_ops = { 'verify' }; good(nil, k)
good(nil, '{"keys":[{"kid":"key","kty":"RSA","n":"AQAB","e":"AQAB"}]}')
bad(nil, '{"keys":[{"kid":"key","kid":"other"}]}')
bad(nil, '{"keys":{}}')

bad(claim('https://auth.openai.com', 'https://evil.invalid'))
bad(claim('"client"', '"other"'))
bad(claim('"user"', '""'))
bad(claim('"fresh"', '"stale"'))
bad(claim(',"nonce":"fresh"', ''))
bad(claim('1100', '1000'))
bad(claim('900', '1001'))
bad(claim('900', '"900"'))
bad(claim('1100', 'null'))
bad(claim('1100', '1e999'))
bad(claim('1100', '800'))
bad(token(nil, claims:gsub('}$', ',"nbf":1001}')))
bad(token(nil, claims:gsub('}$', ',"nbf":null}')))
good(token(nil, claims:gsub('}$', ',"nbf":999}')))
bad(claim('"aud":"client"', '"aud":["client","other"]'))
good(claim('"aud":"client"', '"aud":["client","other"],"azp":"client"'))
bad(claim('"aud":"client"', '"aud":["client","other"],"azp":"other"'))
bad(claim('"aud":"client"', '"aud":"client","azp":"other"'))
bad(claim('"aud":"client"', '"aud":["client","client"]'))
bad(claim('"aud":"client"', '"aud":[]'))
good(claim('"aud":"client"', '"aud":["client"]'))
local e = expected(); e.subject = 'other'; bad(nil, nil, e)
e.subject = 'user'; good(nil, nil, e)
e.nonce = nil; good(claim(',"nonce":"fresh"', ''), nil, e)
e.subject = nil; bad(nil, nil, e)
e = expected(); e.issuer = 'https://evil.invalid'; bad(nil, nil, e)
e = expected(); e.now = math.huge; bad(nil, nil, e)
e = expected(); e.clock_skew = -1; bad(nil, nil, e)
e.clock_skew = 301; bad(nil, nil, e)
e.clock_skew = 10; good(claim('1100', '995'), nil, e)
bad(claim('1100', '990'), nil, e)

for _, malformed in ipairs({ '', 'a.b.c', token() .. '.extra', token() .. '=',
    'a..c', string.rep('a', 16385), 'e30.e30.c2ln', 'e31.e30.c2ln' }) do bad(malformed) end
bad(token('[]'))
bad(token(nil, 'null'))
bad(token('{"alg":"HS256","alg":"RS256","kid":"key"}'))
bad(token('{"alg":"RS256","\\u0061lg":"RS256","kid":"key"}'))
bad(token(nil, claims:gsub('}$', ',"\\u0065xp":1200}')))

for _, text in ipairs({ '{"a":1,"a":2}', '{"a":1,"\\u0061":2}',
    '{"x":{"sub":1,"sub":2}}', '[1,]', '{"a":1,}', '01', '-01', '1.',
    '1e', '+1', 'NaN', 'Infinity', '1e999', 'true false', '"\\uD800"',
    '"\\uDC00"', '"\\uD800\\u0041"', '"\\x41"', '"\n"', '"' .. string.char(255) .. '"',
    string.rep('[', 34) .. '0' .. string.rep(']', 34), string.rep(' ', 1048577) }) do
    local result, err = oidc.decode_json(text)
    assert(result == nil and err == 'invalid JSON')
    checks = checks + 1
end
local parsed = assert(oidc.decode_json(' {"a":null,"b":[true,false,-1.25e2],"s":"\\uD83D\\uDE00"} '))
assert(parsed.a == oidc.null and parsed.b[1] == true and parsed.b[2] == false)
assert(parsed.b[3] == -125 and parsed.s == utf8.char(0x1F600))
assert(oidc.decode_json('false') == false)
assert(oidc.decode_json('null') == oidc.null)
print('OIDC tests passed (' .. checks .. ' validation cases)')
