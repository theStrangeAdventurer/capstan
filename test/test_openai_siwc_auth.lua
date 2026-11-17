-- Offline only: native crypto/IO and HTTP are mocked; OIDC policy is real.
-- Run: vendor/lua-5.5.0/src/lua test/test_openai_siwc_auth.lua
package.path = './?.lua;./?/init.lua;' .. package.path
local json = require('vendor.rxi.json')
local auth = require('agent.openai_siwc_auth')
local issuer = 'https://auth.openai.com'
local token = 'aGVhZA.Y2xhaW0.c2ln'
local disk, locked, closed, write_fail, read_fail, serial, clock
local callback, params, display, requests, claims, response, metadata, denial, bad_state, duplicate, missing_client
local returning_client, invalid_signature, pending_polls, cancelled, refreshes, remote_status, store_busy
local function decode(s)
    return (s:gsub('+', ' '):gsub('%%(%x%x)', function(h) return string.char(tonumber(h, 16)) end))
end
local function parse(s)
    local t = {}; for k, v in s:gmatch('([^&=]+)=([^&]*)') do t[decode(k)] = decode(v) end; return t
end
local function reset()
    disk, locked, closed, write_fail, read_fail, serial, clock = nil, false, 0, false, false, 0, 0
    callback, params, display, requests = nil, nil, nil, {}
    denial, bad_state, duplicate, missing_client, returning_client = false, false, false, false, nil
    invalid_signature, pending_polls, cancelled, refreshes, remote_status, store_busy = false, 0, false, 0, 200, 0
    claims = {iss = issuer, sub = 'fixture-sub', aud = 'issued-fixture', iat = os.time(), exp = os.time() + 3600}
    response = {access_token = 'fixture-access', refresh_token = 'fixture-refresh', id_token = token,
        token_type = 'Bearer', expires_in = 3600,
        scope = 'openid offline_access resource.invoke chatgpt.tokens.use.direct'}
    metadata = {issuer = issuer, jwks_uri = issuer .. '/keys', revocation_endpoint = issuer .. '/revoke'}
end
capstan = {
    state_ensure_dir = function() return true end,
    state_path = function(p) assert(p == 'siwc'); return 'fixture/siwc' end,
    now_ms = function() return clock end,
    oauth = {
        random_base64url = function(n) serial = serial + 1; return string.rep('A', n == 16 and 21 or 42) .. tostring(serial) end,
        base64url_decode = function(s)
            if s == 'aGVhZA' then return json.encode({alg = 'RS256', kid = 'fixture-key'}) end
            if s == 'Y2xhaW0' then return json.encode(claims) end
            return string.rep('\1', 16)
        end,
        pkce_s256 = function(v) assert(#v >= 43); return 'fixture-challenge' end,
        verify_rs256 = function() return not invalid_signature end,
        store_open = function(path)
            assert(path == 'fixture/siwc')
            local held = false
            return {
                try_lock = function()
                    if store_busy > 0 then store_busy = store_busy - 1; return false end
                    assert(not locked); locked, held = true, true; return true
                end,
                read = function() assert(held); if read_fail then return nil, 'fixture secret error' end; return disk end,
                write = function(_, value)
                    assert(held); if write_fail then return nil, 'fixture secret error' end
                    disk = value; return true
                end,
                close = function() if held then locked = false end; closed = closed + 1; return true end,
            }
        end,
        listen = function()
            assert(not locked)
            return {
                poll = function()
                    assert(not locked)
                    if pending_polls > 0 then pending_polls = pending_polls - 1; return nil end
                    return callback
                end,
                close = function() closed = closed + 1 end,
            }, 'http://127.0.0.1:12345/auth/callback'
        end,
    },
}
http = {
    wait_frame = function() clock = clock + 100 end,
    get = function(url, _, options)
        assert(options.https_only == true and options.follow_redirects == false)
        requests[#requests + 1] = url
        if url == issuer .. '/.well-known/openid-configuration' then return 200, json.encode(metadata) end
        assert(url == issuer .. '/keys')
        return 200, json.encode({keys = {{kid = 'fixture-key', kty = 'RSA', n = 'YQ', e = 'AQAB'}}})
    end,
    post_response = function(url, body, headers, timeout, options)
        assert(options.https_only == true and options.follow_redirects == false and timeout == 30000)
        assert(headers['Content-Type'] == 'application/x-www-form-urlencoded')
        requests[#requests + 1] = url
        local f = parse(body)
        if url == issuer .. '/revoke' then
            assert(locked and f.token_type_hint == 'refresh_token' and f.client_id == 'issued-fixture')
            return {status = remote_status, body = ''}
        end
        assert(url == issuer .. '/api/accounts/oauth/token' and f.client_id == 'issued-fixture')
        assert(f.resource == 'https://api.openai.com/v1' and not f.scope)
        if f.grant_type == 'refresh_token' then
            assert(locked and f.refresh_token == 'fixture-refresh'); refreshes = refreshes + 1
        else
            assert(not locked and f.grant_type == 'authorization_code')
            assert(f.redirect_uri == params.redirect_uri and f.code_verifier and f.code == 'fixture-code')
        end
        return {status = 200, body = json.encode(response)}
    end,
}
local ctx = {
    cancelled = function() return cancelled end,
    display_auth = function(label, url)
        assert(not locked and label == 'Continue with ChatGPT' and not url:find('id_token_hint', 1, true))
        display = url; params = parse(url:match('%?(.*)'))
        assert(params.ext_agent_host_id:match('^urn:uuid:.*%-4...%-[89ab]'))
        assert(params.code_challenge_method == 'S256' and params.code_challenge == 'fixture-challenge')
        claims.nonce = params.nonce
        callback = 'state=' .. (bad_state and 'wrong' or params.state)
        if denial then callback = callback .. '&error=access_denied'
        else callback = callback .. '&code=fixture-code' end
        if not missing_client then callback = callback .. '&client_id=' .. (returning_client or 'issued-fixture') end
        if duplicate then callback = callback .. '&state=' .. params.state end
    end,
}
local function rejected(fn, needle)
    local ok, err = fn()
    assert(ok == nil and type(err) == 'string', tostring(err))
    if needle then assert(err:find(needle, 1, true), err) end
    assert(not err:find('fixture secret', 1, true) and not err:find('fixture-access', 1, true))
    assert(not locked)
end
local function account() local rows = assert(auth.status()); return assert(rows[1]).account_id end
local function expire()
    local s = json.decode(disk); s.accounts[s.active].expires_at = os.time() - 1; disk = json.encode(s)
end
reset(); store_busy = 2; pending_polls = 2
assert(auth.authorize(ctx)); assert(clock >= 400)
local id, host = account(), json.decode(disk).host_id
assert(params.client_id == 'dynamic_agent_client' and params.agent_name_hint == 'Capstan')
local access = assert(auth.access()); assert(access.access == 'fixture-access' and access.account_id == id and access.generation == 1)
local rows = assert(auth.status()); assert(rows[1].authenticated and rows[1].active)
assert(not json.encode(rows):find('fixture-access', 1, true) and not json.encode(rows):find(token, 1, true))
missing_client = true; assert(auth.authorize(ctx, id)); assert(params.client_id == 'issued-fixture' and not params.agent_name_hint)
assert(json.decode(disk).host_id == host and account() == id)
local before = disk
returning_client, missing_client = 'wrong-client', false
rejected(function() return auth.authorize(ctx, id) end, 'mismatch'); assert(disk == before)
returning_client = nil; claims.sub = 'wrong-sub'
rejected(function() return auth.authorize(ctx, id) end, 'validation'); assert(disk == before)
for _, mode in ipairs({'denied', 'state', 'duplicate', 'client', 'scope', 'signature', 'audience', 'issuer', 'expired', 'untrusted'}) do
    reset()
    if mode == 'denied' then denial = true
    elseif mode == 'state' then bad_state, denial = true, true
    elseif mode == 'duplicate' then duplicate = true
    elseif mode == 'client' then missing_client = true
    elseif mode == 'scope' then response.scope = 'openid'
    elseif mode == 'signature' then invalid_signature = true
    elseif mode == 'audience' then claims.aud = 'wrong'
    elseif mode == 'issuer' then claims.iss = 'https://evil.invalid'
    elseif mode == 'expired' then claims.exp = os.time() - 1000
    elseif mode == 'untrusted' then metadata.jwks_uri = 'https://auth.openai.com.evil.invalid/keys' end
    rejected(function() return auth.authorize(ctx) end)
    assert(json.decode(disk).active == nil)
    if denial or duplicate or missing_client then assert(#requests == 0) end
end
reset(); assert(auth.authorize(ctx)); id = account(); expire()
response.access_token, response.refresh_token, response.id_token = 'rotated-access', 'rotated-refresh', nil
assert(auth.access(id).generation == 2 and refreshes == 1)
assert(auth.access(id).access == 'rotated-access' and refreshes == 1)
assert(json.decode(disk).accounts[id].refresh_token == 'rotated-refresh')
reset(); assert(auth.authorize(ctx)); expire(); write_fail = true
rejected(function() return auth.access() end, 'storage write failed'); assert(refreshes == 1)
reset(); assert(auth.authorize(ctx)); expire(); response.refresh_token = nil
rejected(function() return auth.access() end, 'Invalid SIWC token response')
reset(); assert(auth.authorize(ctx)); expire(); response.scope = nil
rejected(function() return auth.access() end, 'scopes')
reset(); assert(auth.authorize(ctx)); expire(); claims.sub = 'wrong'
rejected(function() return auth.access() end, 'validation')
reset(); assert(auth.authorize(ctx)); id = account(); host = json.decode(disk).host_id
local ok, message = auth.logout(id); assert(ok and message:find('confirmed', 1, true))
local s = json.decode(disk); assert(s.host_id == host and s.accounts[id].client_id == 'issued-fixture')
assert(not s.accounts[id].access_token and not s.accounts[id].refresh_token and not s.accounts[id].id_token)
rejected(function() return auth.access(id) end, 'signed out')
missing_client = true; assert(auth.authorize(ctx, id)); assert(account() == id)
remote_status = 503; ok, message = auth.logout(id)
assert(ok and message:find('unconfirmed', 1, true) and clock >= 250)
reset(); assert(auth.authorize(ctx)); write_fail = true
rejected(function() return auth.logout() end, 'storage write failed')
reset(); disk = '{broken'; rejected(auth.status, 'Invalid SIWC storage'); assert(disk == '{broken')
reset(); read_fail = true; rejected(auth.status, 'storage read failed')
reset(); cancelled = true; rejected(function() return auth.authorize(ctx) end, 'cancelled')
reset(); pending_polls = 4000; rejected(function() return auth.authorize(ctx) end, 'timed out')
-- Additional trust boundaries: refresh nonce, malformed callback, HTTP failures.
reset(); assert(auth.authorize(ctx)); expire(); claims.nonce = 'wrong-nonce'
rejected(function() return auth.access() end, 'nonce mismatch')
reset(); assert(auth.authorize(ctx)); expire(); claims.nonce = nil
assert(auth.access().generation == 2)
for _, suffix in ipairs({'&state=duplicate', '&%73tate=duplicate', '&code=%GG', '&code=%00', '&broken', '&'}) do
    reset()
    local original = ctx.display_auth
    local badctx = {display_auth = function(label, url) original(label, url); callback = callback .. suffix end}
    rejected(function() return auth.authorize(badctx) end)
    assert(#requests == 0)
end
reset(); assert(auth.authorize(ctx)); before = disk
write_fail = true
rejected(function() return auth.authorize(ctx, account()) end, 'storage write failed')
assert(disk == before)
reset(); assert(auth.authorize(ctx)); before = disk
local original_post = http.post_response
http.post_response = function() return {status = 302, body = 'fixture secret access_token'} end
rejected(function() return auth.authorize(ctx, account()) end, 'exchange failed')
assert(disk == before)
http.post_response = original_post
reset(); assert(auth.authorize(ctx)); id = account()
claims.sub = 'second-account'; assert(auth.authorize(ctx)); local second = account()
-- status is sorted; select active explicitly, not by row position.
for _, row in ipairs(auth.status()) do if row.active then second = row.account_id end end
assert(second ~= id and #auth.status() == 2)
assert(auth.logout(id)); assert(auth.access(second).account_id == second)
assert(not auth.status()[1].access_token)
print('openai_siwc_auth: passed (offline authorization, strict claims, rotation, storage, logout)')
