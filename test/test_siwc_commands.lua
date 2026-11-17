-- Offline: vendor/lua-5.5.0/src/lua test/test_siwc_commands.lua
package.path = './?.lua;' .. package.path
local writes, removes = 0, 0
local legacy_fail, managed_fail
local credential = {type = 'oauth'}
package.loaded['agent.auth'] = {
    set = function(provider, value)
        assert(provider == 'legacy' and value == credential)
        writes = writes + 1
        if legacy_fail then return nil, 'save error' end
        return true
    end,
    remove = function(provider)
        assert(provider == 'legacy')
        removes = removes + 1
        if legacy_fail then return nil, 'remove error' end
        return true
    end,
    list = function() return {legacy = credential} end,
    redacted = function() return {type = 'oauth', expires = 60000} end,
}
capstan = {now_ms = function() return 0 end}
local displayed
os.getenv = function() return nil end -- Never inspect real keys in command tests.
popup = {info = function(label, text) assert(label == 'Continue with ChatGPT'); displayed = text end,
    choice = function(_, _, choices)
        assert(#choices == 2 and choices[1] == 'OpenCode Go' and choices[2] == 'ChatGPT / Codex')
        return choices[2]
    end}
local seen_ctx, seen_account
package.loaded['agent.openai_siwc_auth'] = {
    authorize = function(ctx, account)
        seen_ctx, seen_account = ctx, account
        ctx.display_auth('Continue with ChatGPT', 'https://auth.openai.com/fixture')
        assert(displayed:find('https://auth.openai.com/fixture', 1, true))
        if managed_fail then return nil, 'authorize error' end
        return true
    end,
    logout = function(account)
        seen_account = account
        if managed_fail then return nil, 'logout error' end
        return true, 'Managed logout complete'
    end,
    status = function()
        if managed_fail then return nil, 'status error' end
        return {{account_id = 'account-a', email = 'a@example.test', authenticated = true,
            access_token = 'DO_NOT_SHOW', refresh_token = 'DO_NOT_SHOW', other = 'DO_NOT_SHOW'}}
    end,
}
local siwc = dofile('plugins/openai_siwc.lua')
local method
plugins = {siwc, {auth = {provider = 'legacy', authorize = function(value)
    method = value
    if legacy_fail == 'authorize' then return nil, 'legacy authorize error' end
    return credential
end}}}
local connect = dofile('plugins/connect.lua')
local logout = dofile('plugins/logout.lua')
local status = dofile('plugins/auth.lua')
local function context(...)
    return {args = {...}, replace = function(_, text) return text, text end}
end
for _, p in ipairs({connect, logout, status}) do
    assert(p.history == false and p.tool == nil and p.tools == nil)
end
assert(siwc.auth.default_method == 'browser' and siwc.auth.persistence == 'managed')
assert(connect.handler(context()) == 'Connected: openai_siwc')
local ctx = context('openai_siwc', 'account-a')
assert(connect.handler(ctx) == 'Connected: openai_siwc')
assert(seen_ctx == ctx and seen_account == 'account-a' and writes == 0)
connect.handler(context('openai_siwc'))
assert(seen_account == nil and writes == 0)
assert(logout.handler(context('openai_siwc', 'account-a')) == 'Managed logout complete')
assert(seen_account == 'account-a' and removes == 0)
logout.handler(context('openai_siwc'))
assert(seen_account == nil and removes == 0)
local text = status.handler(context())
assert(text:find('id=account-a', 1, true) and text:find('email=a@example.test', 1, true))
assert(text:find('connected=true', 1, true) and text:find('legacy: oauth expires=1m', 1, true))
assert(not text:find('DO_NOT_SHOW', 1, true) and not text:find('access_token', 1, true))
managed_fail = true
for _, p in ipairs({connect, logout, status}) do
    local message, _, ok = p.handler(context('openai_siwc'))
    assert(ok == false and message:find('error', 1, true))
end
assert(writes == 0 and removes == 0)
managed_fail = false
assert(connect.handler(context('legacy')) == 'Connected: legacy')
assert(method == 'device' and writes == 1)
connect.handler(context('legacy', 'custom'))
assert(method == 'custom' and writes == 2)
assert(logout.handler(context('legacy')) == 'Logged out: legacy' and removes == 1)
legacy_fail = 'authorize'
local _, _, ok = connect.handler(context('legacy'))
assert(ok == false and writes == 2)
legacy_fail = true
_, _, ok = connect.handler(context('legacy'))
assert(ok == false and writes == 3)
_, _, ok = logout.handler(context('legacy'))
assert(ok == false and removes == 2)
plugins = {}
package.loaded['agent.auth'].list = function() return {} end
local empty_status = status.handler(context())
assert(empty_status:find('opencode_go: not checked', 1, true))
assert(not empty_status:find('legacy:', 1, true))
print('SIWC command offline tests passed')
