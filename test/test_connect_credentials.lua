-- Offline only: all credentials, storage, environment and HTTP are mocked.
package.path = './?.lua;' .. package.path
local json = require('vendor.rxi.json')
local env, stored, reads, writes = {}, nil, 0, 0
local locked, clock, busy, unreadable, unsafe = false, 0, false, false, false
local refused, selected, secret = false, 1, 'fixture-key'
os.getenv = function(name) return env[name] end
os.setenv = function() error('must not export credentials') end
local legacy_calls = 0
package.loaded['agent.auth'] = {
    set = function(id) assert(id == 'legacy'); legacy_calls = legacy_calls + 1; return true end,
    remove = function() return true end,
    list = function() return {} end,
}
capstan = {config = {}, now_ms = function() return clock end,
    state_ensure_dir = function() return true end,
    state_path = function(name) assert(name == 'credentials'); return 'fixture/credentials' end,
    oauth = {store_open = function(path)
        assert(path == 'fixture/credentials')
        if unsafe then return nil, 'unsafe fixture' end
        local acquired = false
        return {
            try_lock = function()
                if busy then return false end
                assert(not locked); locked, acquired = true, true; return true
            end,
            read = function()
                assert(acquired); reads = reads + 1
                if unreadable then return nil, 'secret must not escape this error' end
                return stored
            end,
            write = function(_, value)
                assert(acquired); writes = writes + 1
                if refused then return nil, 'secret must not escape this error' end
                stored = value; return true
            end,
            close = function() if acquired then locked = false end end,
        }
    end},
}
popup = {
    secret = function() return secret end,
    choice = function(_, _, choices)
        assert(#choices == 2 and choices[1] == 'OpenCode Go' and choices[2] == 'ChatGPT / Codex')
        return choices[selected]
    end,
}
local managed_calls = 0
plugins = {
    {auth = {provider = 'legacy', label = 'Legacy', authorize = function(method)
        assert(method == 'device' or method == 'custom'); return {type = 'oauth'} end}},
    {auth = {provider = 'openai_codex', authorize = function() error('hidden provider selected') end}},
    {auth = {provider = 'openai_siwc', persistence = 'managed', authorize = function()
        managed_calls = managed_calls + 1; return true end}},
}
local connect = dofile('plugins/connect.lua')
local logout = dofile('plugins/logout.lua')
local status = dofile('plugins/auth.lua')
local function ctx(...) return {args = {...}, replace = function(_, text) return text, text end} end
local credentials = require('agent.credentials')
local provider = {endpoint = credentials.definition('deepseek').endpoint, credential_provider = 'deepseek'}
local runtime = require('agent.provider_config').build()
assert(reads == 0 and runtime.providers.deepseek.api_key == nil)
assert(status.handler(ctx()):find('not connected', 1, true) and writes == 0 and not locked)
assert(connect.history == false and logout.history == false and status.history == false and connect.tool == nil)
local text = connect.handler(ctx('deepseek'))
assert(text:find('Connected: deepseek', 1, true) and not text:find(secret, 1, true))
assert(credentials.resolve('deepseek', provider) == secret)
assert(runtime.providers.deepseek.api_key == nil) -- Existing runtime uses new key without copying it.
local expected = secret
http = {get = function(url, headers)
    assert(url == provider.endpoint:gsub('/chat/completions$', '/models'))
    assert(headers.Authorization == 'Bearer ' .. expected)
    return 200, '{"data":[{"id":"fixture-model"}]}'
end}
assert(require('agent.models').list(runtime, 'deepseek')[1].id == 'fixture-model')
env.DEEPSEEK_API_KEY = 'fixture-env'
assert(credentials.resolve('deepseek', provider) == secret) -- Explicit connection wins now.
package.loaded['agent.credentials'] = nil
credentials = require('agent.credentials')
assert(credentials.resolve('deepseek', provider) == 'fixture-env') -- Environment wins next launch.
env.DEEPSEEK_API_KEY = nil
local read_before = reads
assert(credentials.resolve('deepseek', provider) == secret and reads == read_before + 1)
assert(credentials.resolve('deepseek', provider) == secret and reads == read_before + 2)
assert(credentials.resolve('deepseek', {endpoint = 'https://custom.invalid'}) == nil)
assert(credentials.resolve('deepseek', provider, 'https://custom.invalid/models') == nil)
assert(credentials.resolve('deepseek', {endpoint = 'https://custom.invalid', api_key = 'fixture-config'}) == 'fixture-config')
local old, snapshot = secret, stored
secret, refused = 'fixture-replacement', true
local failed, _, success = connect.handler(ctx('deepseek'))
assert(success == false and failed:find('failed', 1, true))
assert(stored == snapshot and not locked and not failed:find(secret, 1, true))
assert(credentials.resolve('deepseek', provider) == old)
refused = false
selected = 1
env.DEEPSEEK_API_KEY = 'fixture-env'
text = connect.handler(ctx('deepseek'))
assert(text:find('future launches', 1, true) and not text:find(secret, 1, true))
assert(credentials.resolve('deepseek', provider) == secret)
expected = secret
assert(require('agent.models').list(runtime, 'deepseek'))
assert(not status.handler(ctx()):find(secret, 1, true))
local before = writes
assert(connect.handler(ctx('deepseek', 'accidental-key')):find('Do not pass', 1, true))
assert(writes == before)
assert(not connect.handler(ctx('accidental-key')):find('accidental-key', 1, true))
assert(logout.handler(ctx('deepseek')):find('unchanged', 1, true))
assert(credentials.resolve('deepseek', provider) == 'fixture-env' and next(json.decode(stored).providers) == nil)
env.DEEPSEEK_API_KEY = nil
assert(credentials.resolve('deepseek', provider) == nil)
assert(connect.handler(ctx('deepseek')):find('file', 1, true))
assert(credentials.resolve('deepseek', provider) == secret)
secret = nil
assert(connect.handler(ctx('deepseek')) == 'Connection cancelled')
assert(credentials.resolve('deepseek', provider) == expected)
-- Read-modify-write retains other providers, and reads observe another process.
secret = 'fixture-go-key'
assert(connect.handler(ctx('opencode_go')):find('Connected:', 1, true))
assert(json.decode(stored).providers.deepseek.api_key == expected)
assert(logout.handler(ctx('opencode_go')):find('Stored key removed', 1, true))
assert(json.decode(stored).providers.deepseek.api_key == expected)
local good = stored
local external = json.decode(stored)
external.providers.deepseek.api_key = 'fixture-external-update'
stored = json.encode(external)
assert(credentials.resolve('deepseek', provider) == 'fixture-external-update')
external.providers.deepseek = nil; stored = json.encode(external)
assert(credentials.resolve('deepseek', provider) == nil)
stored = good
secret = 'fixture-new-key'
for _, bad in ipairs({'{broken', '{"version":1,"version":1,"providers":{}}',
    '{"version":1,"providers":{"deepseek":{"endpoint":"https://wrong.invalid","api_key":"fixture"}}}'}) do
    stored = bad
    local count = writes
    local message, _, ok = connect.handler(ctx('deepseek'))
    assert(ok == false and stored == bad and writes == count and not locked)
    assert(not message:find(secret, 1, true))
    assert(not pcall(credentials.resolve, 'deepseek', provider))
end
stored = good
for _, mode in ipairs({'unsafe', 'unreadable', 'busy', 'refused'}) do
    unsafe, unreadable, busy, refused = mode=='unsafe', mode=='unreadable', mode=='busy', mode=='refused'
    http.wait_frame = function() clock = clock + 1000 end
    local message, _, ok = connect.handler(ctx('deepseek'))
    assert(ok == false and stored == good and not locked)
    assert(not message:find('secret must not escape', 1, true))
    local logout_message, _, logout_ok = logout.handler(ctx('deepseek'))
    assert(logout_ok == false and stored == good and not locked)
    assert(not logout_message:find(secret, 1, true))
end
unsafe, unreadable, busy, refused = false, false, false, false
assert(credentials.resolve('deepseek', provider) == expected)
assert(connect.handler(ctx('legacy')) == 'Connected: legacy')
assert(connect.handler(ctx('legacy', 'custom')) == 'Connected: legacy' and legacy_calls == 2)
selected = 2
assert(connect.handler(ctx()) == 'Connected: openai_siwc' and managed_calls == 1)
selected = 1
assert(connect.handler(ctx()):find('Connected: opencode_go', 1, true))
-- Exercise the real request adapter as well as the catalog adapter above.
local function noop() end
package.loaded['agent.logging'] = {runtime_log = noop, compact = tostring, safe_error = tostring,
    debug = noop, raw_logging_enabled = function() return false end}
package.loaded['agent.hooks'] = {has = function() return false end,
    run = function(_, value) return value end, install_config = noop, install_existing_plugins = noop}
package.loaded['agent.mcp'] = {}
package.loaded['agent.ui'] = {append = noop}
package.loaded['agent.tasks'] = {refresh = noop}
package.loaded['agent.tools'] = {collect = function() return {} end, names = function() return '' end}
agent = {set_info = noop, set_usage = noop, set_profile_info = noop,
    session_id = function() return 'fixture-session' end}
local request_count = 0
http.post_stream = function(_, _, headers)
    assert(headers.Authorization == 'Bearer ' .. expected)
    request_count = request_count + 1
    return request_count
end
http.cancel = noop
local live = require('agent.runtime')
live.providers.deepseek.context_limit = 1000
assert(live.providers.deepseek.api_key == nil)
secret, selected = 'fixture-immediate-request', 1
assert(connect.handler(ctx('deepseek')):find('Connected:', 1, true))
expected = secret
assert(live.run({provider = 'deepseek', tools = {}, completion_review = false}, {on_text = noop}))
assert(request_count == 1 and live.providers.deepseek.api_key == nil)
local rejected
package.loaded['agent.hooks'].run = function(stage, value)
    if stage == 'before_request' then
        assert(value.headers.Authorization == nil, 'managed key exposed before endpoint validation')
        value.endpoint = 'https://custom.invalid/chat/completions'
    end
    return value
end
live.run({provider = 'deepseek', tools = {}, completion_review = false},
    {on_text = noop, on_done = function(result) rejected = result end})
assert(rejected and not rejected.ok and request_count == 1)
live.shutdown_background()
print('Connect credential offline tests passed')
