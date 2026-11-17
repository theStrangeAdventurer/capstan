-- Official SIWC policy owner. Native helpers own TLS, crypto, loopback and files.
-- ctx.display_auth(label, url) is a UI-only callback (never append to history).
-- ctx.cancelled() may cancel authorization; no shell/browser helpers are used.
local oidc = require('agent.oidc')
local json = require('vendor.rxi.json')
local M = {}
local ISSUER = 'https://auth.openai.com'
local TOKEN = ISSUER .. '/api/accounts/oauth/token'
local RESOURCE = 'https://api.openai.com/v1'
local SCOPES = 'openid profile email offline_access resource.invoke chatgpt.tokens.use.direct'
local OPTIONS = {https_only = true, follow_redirects = false}
local function fail(message) error({siwc_error = message}, 0) end
local function need(ok, message) if not ok then fail(message) end; return ok end
local function text(s) return type(s) == 'string' and #s > 0 and #s <= 16384 and not s:find('[%z\1-\31\127]') end
local function integer(n) return type(n) == 'number' and n >= 0 and n < 1e15 and n % 1 == 0 end
local function object(t) return type(t) == 'table' and t ~= oidc.null end
local function guarded(fn)
    local ok, a, b = pcall(fn)
    if ok then return a, b end
    return nil, type(a) == 'table' and a.siwc_error or 'SIWC operation failed; no credentials were returned'
end
local function now() return capstan.now_ms() end
local function cancelled(ctx)
    return ctx and (ctx.cancelled == true or (type(ctx.cancelled) == 'function' and ctx.cancelled()))
end
local function frame(ctx, deadline)
    need(not cancelled(ctx), 'Sign-in cancelled')
    need(now() < deadline, 'SIWC operation timed out')
    http.wait_frame()
end
local function random() return need(capstan.oauth.random_base64url(32), 'Secure randomness unavailable') end
local function uuid()
    local raw = need(capstan.oauth.base64url_decode(need(capstan.oauth.random_base64url(16), 'Secure randomness unavailable')), 'Secure randomness unavailable')
    need(#raw == 16, 'Secure randomness unavailable')
    local b = {raw:byte(1, 16)}
    b[7], b[9] = (b[7] & 15) | 64, (b[9] & 63) | 128
    local h = {}; for i, v in ipairs(b) do h[i] = string.format('%02x', v) end
    return 'urn:uuid:' .. table.concat(h, '', 1, 4) .. '-' .. table.concat(h, '', 5, 6)
        .. '-' .. table.concat(h, '', 7, 8) .. '-' .. table.concat(h, '', 9, 10) .. '-' .. table.concat(h, '', 11, 16)
end
local function valid_host(s)
    return type(s) == 'string' and s:match('^urn:uuid:%x%x%x%x%x%x%x%x%-%x%x%x%x%-4%x%x%x%-[89ab]%x%x%x%-%x%x%x%x%x%x%x%x%x%x%x%x$')
end
local function scopes(s)
    need(text(s), 'Missing granted scopes; authorize again')
    local out = {}; for v in s:gmatch('%S+') do out[v] = true end
    need(out['chatgpt.tokens.use.direct'] and out['resource.invoke'] and out.openid and out.offline_access,
        'ChatGPT plan permission is missing; authorize again and enable plan usage')
    return s
end
local function validate_store(s)
    need(object(s) and s.version == 1 and valid_host(s.host_id) and object(s.accounts), 'Invalid SIWC storage')
    local identities = {}
    for id, r in pairs(s.accounts) do
        need(type(id) == 'string' and id:match('^[%w_-]+$') and object(r) and r.account_id == id
            and r.issuer == ISSUER and text(r.subject) and text(r.client_id) and r.client_id ~= 'dynamic_agent_client'
            and integer(r.generation) and r.generation > 0
            and (r.email == nil or (text(r.email) and #r.email <= 254))
            and (r.auth_nonce == nil or text(r.auth_nonce))
            and (r.earliest_refresh_at == nil or integer(r.earliest_refresh_at)), 'Invalid SIWC storage')
        local key = json.encode({r.issuer, r.subject, r.client_id})
        need(not identities[key], 'Invalid SIWC storage'); identities[key] = true
        if r.access_token ~= nil then
            need(text(r.access_token) and text(r.refresh_token) and text(r.id_token)
                and r.token_type == 'Bearer' and integer(r.expires_at) and integer(r.saved_at), 'Invalid SIWC storage')
            scopes(r.scope)
        else
            need(r.refresh_token == nil and r.id_token == nil, 'Invalid SIWC storage')
        end
    end
    need(s.active == nil or (type(s.active) == 'string' and s.accounts[s.active]), 'Invalid SIWC storage')
    return s
end
local function save(h, s)
    need(h:write(json.encode(s)), 'SIWC storage write failed; authorize again before using credentials')
end
local function locked(ctx, fn)
    need(capstan.state_ensure_dir(), 'SIWC state directory unavailable')
    local path = need(capstan.state_path('siwc'), 'SIWC state path unavailable')
    local h = need(capstan.oauth.store_open(path), 'SIWC storage unavailable')
    local ok, a, b = pcall(function()
        local deadline = now() + 10000
        while true do
            need(not cancelled(ctx), 'Sign-in cancelled')
            local acquired = h:try_lock()
            need(acquired ~= nil, 'SIWC storage lock failed')
            if acquired then break end
            frame(ctx, deadline)
        end
        local raw, err = h:read()
        need(not err, 'SIWC storage read failed')
        local s
        if raw == nil then s = {version = 1, host_id = uuid(), accounts = {}}; save(h, s)
        else s = validate_store(need(oidc.decode_json(raw), 'Invalid SIWC storage')) end
        return fn(s, h)
    end)
    h:close() -- also releases locks on every error path
    if not ok then error(a, 0) end
    return a, b
end
local function encode(s) return (s:gsub('[^A-Za-z0-9_.~-]', function(c) return string.format('%%%02X', c:byte()) end)) end
local function form(fields)
    local out = {}; for k, v in pairs(fields) do out[#out + 1] = encode(k) .. '=' .. encode(v) end
    table.sort(out); return table.concat(out, '&')
end
local function query(raw)
    need(type(raw) == 'string' and #raw <= 16384 and raw ~= '', 'Invalid authorization callback')
    local out = {}
    local function decode(v)
        need(not v:gsub('%%[%x][%x]', ''):find('%%'), 'Invalid authorization callback')
        v = v:gsub('+', ' '):gsub('%%(%x%x)', function(h) return string.char(tonumber(h, 16)) end)
        need(not v:find('[%z\1-\31\127]'), 'Invalid authorization callback'); return v
    end
    for part in (raw .. '&'):gmatch('(.-)&') do
        local k, v = part:match('^([^=]+)=(.*)$')
        need(k, 'Invalid authorization callback'); k, v = decode(k), decode(v)
        need(out[k] == nil, 'Duplicate authorization callback parameter'); out[k] = v
    end
    return out
end
local function trusted(url)
    -- Fail closed to issuer origin until another official origin is explicitly approved.
    return type(url) == 'string' and url:match('^https://auth%.openai%.com/[%w/_.~-]+$') ~= nil
end
local function get(url)
    local status, body, err = http.get(url, {}, OPTIONS)
    need(not err and status == 200, 'SIWC discovery or signing keys unavailable')
    local value = oidc.decode_json(body)
    return need(object(value) and value, 'Invalid SIWC discovery or signing keys')
end
local function discovery()
    local d = get(ISSUER .. '/.well-known/openid-configuration')
    need(d.issuer == ISSUER and trusted(d.jwks_uri)
        and (d.token_endpoint == nil or d.token_endpoint == TOKEN)
        and (d.authorization_endpoint == nil or d.authorization_endpoint == ISSUER .. '/api/accounts/authorize')
        and (d.revocation_endpoint == nil or trusted(d.revocation_endpoint)), 'Untrusted SIWC discovery endpoint')
    return d
end
local function post(url, fields)
    return http.post_response(url, form(fields), {['Content-Type'] = 'application/x-www-form-urlencoded'}, 30000, OPTIONS)
end
local function exchange(fields)
    local r = post(TOKEN, fields)
    need(object(r) and not r.error and r.status == 200, 'SIWC token exchange failed; authorize again')
    local t = oidc.decode_json(r.body)
    need(object(t) and text(t.access_token) and text(t.refresh_token)
        and t.token_type == 'Bearer' and integer(t.expires_in) and t.expires_in > 0 and t.expires_in <= 3600,
        'Invalid SIWC token response; authorize again')
    scopes(t.scope) -- Missing scope is not silently inferred on refresh.
    if t.earliest_refresh_at ~= nil then
        need(integer(t.earliest_refresh_at) and t.earliest_refresh_at <= os.time() + t.expires_in,
            'Invalid SIWC refresh time')
    end
    return t
end
local function claims(token, client, nonce, subject)
    need(text(token), 'Missing SIWC ID token')
    local d = discovery()
    local expected = {issuer = ISSUER, audience = client, nonce = nonce, subject = subject, clock_skew = 60}
    local c = oidc.validate(token, get(d.jwks_uri), expected)
    -- One fresh fetch also covers an issuer key rotation; never trust token URLs.
    if not c then c = oidc.validate(token, get(d.jwks_uri), expected) end
    return need(c, 'SIWC ID token validation failed')
end
local function update(r, t, c)
    r.access_token, r.refresh_token = t.access_token, t.refresh_token
    r.id_token = t.id_token or r.id_token
    r.token_type, r.scope = t.token_type, t.scope
    r.saved_at = os.time(); r.expires_at = r.saved_at + t.expires_in
    r.earliest_refresh_at = t.earliest_refresh_at
    r.generation = r.generation + 1
    if c and type(c.email) == 'string' and #c.email <= 254 and not c.email:find('[%z\1-\31\127]') then r.email = c.email end
end
local function selected(s, id)
    need(id == nil or type(id) == 'string', 'Invalid SIWC account')
    return need(s.accounts[id or s.active], 'Choose a saved ChatGPT account or authorize a new one')
end
function M.authorize(ctx, account_id)
    return guarded(function()
        need(type(ctx) == 'table' and type(ctx.display_auth) == 'function', 'Continue with ChatGPT requires a UI display callback')
        local host, pending = locked(ctx, function(s)
            return s.host_id, account_id ~= nil and selected(s, account_id) or nil
        end)
        local state, nonce, verifier = random(), random(), random()
        local challenge = need(capstan.oauth.pkce_s256(verifier), 'PKCE unavailable')
        local listener, redirect = capstan.oauth.listen()
        need(listener and text(redirect), 'Loopback listener unavailable')
        local ok, result = pcall(function()
            local params = {client_id = pending and pending.client_id or 'dynamic_agent_client',
                agent_name_hint = not pending and 'Capstan' or nil, ext_agent_host_id = host,
                response_type = 'code', redirect_uri = redirect, scope = SCOPES, resource = RESOURCE,
                state = state, nonce = nonce, code_challenge_method = 'S256', code_challenge = challenge}
            -- Hints are optional: deliberately omit them from the displayed URL.
            ctx.display_auth('Continue with ChatGPT', ISSUER .. '/api/accounts/authorize?' .. form(params))
            local deadline, raw = now() + 300000
            repeat
                need(not cancelled(ctx), 'Sign-in cancelled')
                local err; raw, err = listener:poll()
                -- Native listener ignores unrelated HTTP requests; only a valid callback consumes it.
                if err and err ~= 'invalid callback request' and err ~= 'callback connection closed'
                    and err ~= 'callback request timed out' then fail('Loopback authorization failed') end
                if not raw then frame(ctx, deadline) end
            until raw
            local q = query(raw)
            need(q.state == state, 'Authorization state mismatch')
            need(q.error == nil, q.error == 'access_denied' and 'ChatGPT authorization denied' or 'ChatGPT authorization failed')
            need(text(q.code), 'Missing authorization code')
            local client = q.client_id or (pending and pending.client_id)
            need(text(client) and client ~= 'dynamic_agent_client', 'Missing issued client ID')
            need(not pending or client == pending.client_id, 'Issued client ID mismatch')
            local t = exchange({grant_type = 'authorization_code', client_id = client, code = q.code,
                code_verifier = verifier, redirect_uri = redirect, resource = RESOURCE})
            local c = claims(t.id_token, client, nonce, pending and pending.subject)
            need(not cancelled(ctx), 'Sign-in cancelled')
            return locked(ctx, function(s, h)
                need(s.host_id == host, 'SIWC host changed during authorization')
                local r
                if pending then
                    r = selected(s, account_id)
                    need(r.generation == pending.generation and r.subject == c.sub and r.client_id == client,
                        'SIWC account changed during authorization; try again')
                else
                    for _, existing in pairs(s.accounts) do
                        if existing.issuer == c.iss and existing.subject == c.sub and existing.client_id == client then r = existing end
                    end
                    if not r then
                        local id = random(); need(not s.accounts[id], 'SIWC account ID collision')
                        r = {account_id = id, issuer = c.iss, subject = c.sub, client_id = client, generation = 0}
                    end
                end
                update(r, t, c); r.auth_nonce = nonce
                s.accounts[r.account_id] = r; s.active = r.account_id
                save(h, s); return true
            end)
        end)
        listener:close()
        if not ok then error(result, 0) end
        return result
    end)
end
function M.access(account_id)
    return guarded(function() return locked(nil, function(s, h)
        local r = selected(s, account_id)
        need(r.access_token and r.refresh_token, 'ChatGPT account is signed out; authorize again')
        if r.expires_at <= os.time() + 60 then
            need(not r.earliest_refresh_at or os.time() >= r.earliest_refresh_at, 'ChatGPT token cannot refresh yet; try later')
            local t = exchange({grant_type = 'refresh_token', client_id = r.client_id, refresh_token = r.refresh_token, resource = RESOURCE})
            local c
            if t.id_token ~= nil then
                c = claims(t.id_token, r.client_id, nil, r.subject)
                need(c.nonce == nil or c.nonce == r.auth_nonce, 'SIWC refresh nonce mismatch; authorize again')
            end
            update(r, t, c); save(h, s)
        end
        return {access = r.access_token, account_id = r.account_id, generation = r.generation}
    end) end)
end
function M.status()
    return guarded(function() return locked(nil, function(s)
        local rows = {}
        for id, r in pairs(s.accounts) do
            rows[#rows + 1] = {account_id = id, label = 'ChatGPT ' .. id, email = r.email,
                active = s.active == id, authenticated = r.access_token ~= nil, generation = r.generation}
        end
        table.sort(rows, function(a, b) return a.account_id < b.account_id end)
        return rows
    end) end)
end
function M.logout(account_id)
    return guarded(function() return locked(nil, function(s, h)
        local r = selected(s, account_id)
        local confirmed = false
        if r.refresh_token then
            -- Remote failure must not prevent explicit local sign-out.
            local ok, d = pcall(discovery)
            if ok and d.revocation_endpoint then
                local fields = {token = r.refresh_token, token_type_hint = 'refresh_token', client_id = r.client_id}
                for attempt = 1, 2 do
                    local sent, response = pcall(post, d.revocation_endpoint, fields)
                    if sent and object(response) and not response.error and response.status == 200 then confirmed = true; break end
                    local retry = not sent or not object(response) or response.error or response.status == 0
                        or (type(response.status) == 'number' and response.status >= 500)
                    if not retry or attempt == 2 then break end
                    local until_ms = now() + 250
                    while now() < until_ms do http.wait_frame() end
                end
            end
        end
        r.access_token, r.refresh_token, r.id_token, r.auth_nonce = nil, nil, nil, nil
        r.expires_at, r.saved_at, r.scope, r.token_type, r.earliest_refresh_at = nil, nil, nil, nil, nil
        r.generation = r.generation + 1
        save(h, s)
        return true, confirmed and 'Signed out; remote revocation confirmed'
            or 'Signed out locally; remote revocation unconfirmed. Disconnect the app in ChatGPT Settings.'
    end) end)
end
return M
