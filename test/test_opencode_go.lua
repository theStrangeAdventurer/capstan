-- Offline: vendor/lua-5.5.0/src/lua test/test_opencode_go.lua
package.path = './?.lua;./?/init.lua;' .. package.path
local function noop() end
local env = {}
os.getenv = function(name) return env[name] end -- Never read real credentials.
package.loaded['agent.state'] = {provider = noop, profile_models = function() return {} end,
    model_for = noop, model_reasoning_effort = noop}
capstan = {version = 'test-version', config = {}, workdir = '.', workspace_root = '.'}
local config = require('agent.provider_config')
local built = config.build()
local go = built.providers.opencode_go
assert(go.model == 'deepseek-v4-pro' and go.context_limit == 0)
assert(go.endpoint == 'https://opencode.ai/inference/go/openai/v1/chat/completions')
assert(built.provider == 'deepseek' and built.providers.openrouter.routing.sort == 'throughput')
local models = require('agent.models')
http = {get = function(url, headers)
    assert(url == 'https://opencode.ai/inference/go/v1/models' and headers.Authorization == nil)
    return 200, '{"data":[{"id":"deepseek-v4-pro","type":"chat","go":{},"modalities":{"output":["text"]},"limit":{"context":1000000}}]}'
end}
assert(models.ensure_context_limit(go) == 1000000)
assert(models.list(built, 'opencode_go')[1].id == 'deepseek-v4-pro')
capstan.config = {provider = 'opencode_go', providers = {opencode_go = {
    api_key = 'fake-config', model = 'explicit-model', context_limit = 12345,
    endpoint = 'https://fixture.invalid/chat/completions'}}}
built = config.build()
assert(built.providers.opencode_go.api_key == 'fake-config')
assert(built.providers.opencode_go.model == 'explicit-model')
assert(models.ensure_context_limit(built.providers.opencode_go) == 12345)
env = {OPENCODE_GO_API_KEY = 'fake-env', CAPSTAN_MODEL = 'env-model', CAPSTAN_CONTEXT_LIMIT = '23456'}
built = config.build()
assert(built.providers.opencode_go.api_key == 'fake-env')
assert(built.providers.opencode_go.model == 'env-model')
assert(built.providers.opencode_go.configured_context_limit == 23456)
assert(built.providers.opencode_go.endpoint == 'https://fixture.invalid/chat/completions')
capstan.config.providers.opencode_go.endpoint = nil
package.loaded['agent.logging'] = {runtime_log = noop, compact = tostring, safe_error = tostring,
    debug = noop, raw_logging_enabled = function() return false end}
package.loaded['agent.hooks'] = {has = function() return false end,
    run = function(_, ctx) return ctx end, install_config = noop, install_existing_plugins = noop}
package.loaded['agent.mcp'] = {}
package.loaded['agent.ui'] = {append = noop}
package.loaded['agent.tasks'] = {refresh = noop}
package.loaded['agent.tools'] = {collect = function() return {} end, names = function() return '' end,
    handle_tool_calls = function(messages, available, _, _, continue) continue(messages, available) end}
local visible = 'session-a'
agent = {set_info = noop, set_usage = noop, set_profile_info = noop,
    session_id = function() return visible end}
local requests = {}
local json = require('vendor.rxi.json')
http.post_stream = function(endpoint, body, headers, callback)
    requests[#requests + 1] = {endpoint = endpoint, body = json.decode(body), headers = headers, callback = callback}
    return #requests
end
http.cancel = noop
local runtime = require('agent.runtime')
local function run(opts)
    opts = opts or {}
    opts.tools, opts.completion_review = {}, false
    assert(runtime.run(opts, {on_text = noop}))
    return requests[#requests]
end
local first = run()
assert(first.headers.Authorization == 'Bearer fake-env')
assert(first.headers['User-Agent'] == 'capstan/test-version')
assert(first.headers['x-opencode-session'] == 'session-a')
assert(first.endpoint == go.endpoint and first.body.model == 'env-model')
visible = 'session-b'
-- Continuations use the captured conversation, not the now-visible session.
first.callback('data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c","type":"function","function":{"name":"probe","arguments":"{}"}}]}}]}\n\n', false)
first.callback(nil, true)
assert(requests[#requests].headers['x-opencode-session'] == 'session-a')
for _, opts in ipairs({{background = true, process_owner = 'session-a'},
    {depth = 1, process_owner = 'session-a'}, {purpose = 'title', process_owner = 'session-a'},
    {purpose = 'compact', process_owner = 'session-a'},
    {purpose = 'review', depth = 1, background = true, process_owner = 'session-a'}}) do
    assert(run(opts).headers['x-opencode-session'] == 'session-a')
end
assert(run().headers['x-opencode-session'] == 'session-b')
-- Header policy belongs to the Go provider, never a particular model family.
for _, model in ipairs({'deepseek-v4-pro', 'glm-5.3-flash', 'mimo-v2.6-flash'}) do
    local request = run({model = model, process_owner = 'session-a'})
    assert(request.body.model == model)
    assert(request.headers['x-opencode-session'] == 'session-a')
end
for _, provider in ipairs({'deepseek', 'openrouter'}) do
    runtime.providers[provider].context_limit = 1000
    local request = run({provider = provider})
    assert(request.headers['x-opencode-session'] == nil and request.headers['User-Agent'] == nil)
    assert(request.endpoint == runtime.providers[provider].endpoint)
end
runtime.shutdown_background()
print('OpenCode Go offline tests passed')
