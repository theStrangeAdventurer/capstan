-- Loaded by test_provider_tools.c with an isolated runtime/state fixture.
local models = require('agent.models')
local step = models.step_reasoning_effort
local levels = {'high', 'low', 'medium', 'high', 'invalid'}
assert(step(levels, 'medium', 1) == 'high')
assert(step(levels, 'high', 1) == 'high')
assert(step(levels, 'low', -1) == 'default')
assert(step(levels, nil, -1) == 'default')
assert(step(levels, nil, 1) == 'low')
assert(step({'low', 'high'}, 'medium', 1) == 'high')
assert(step({'low', 'high'}, 'medium', -1) == 'low')
assert(step({}, 'medium', 1) == nil)
assert(step(levels, 'low', 0) == nil)

capstan.config.agent = {profile = 'implement'}
capstan.config.providers.openrouter.models = {
    {id = 'config/model', supported_parameters = {'reasoning'}},
    {id = 'plain', supported_parameters = {'temperature'}},
}
capstan.config.providers.openrouter.default_reasoning_efforts = levels
local runtime = dofile('agent/runtime.lua')
local api = capstan.agent
local info = capstan.models.effective
local original_get = http.get
http.get = function() error('Hotkey must not fetch models') end
assert(info().reasoning_effort == 'medium')
assert(api.step_reasoning_effort(1) == 'high')
assert(info().reasoning_effort == 'high')
assert(capstan.models.profile('implement').reasoning_effort == 'high')
assert(api.step_reasoning_effort(1) == 'high')
assert(api.step_reasoning_effort(-1) == 'medium')
api.set_profile('plan')
assert(info().reasoning_effort == 'high')
assert(api.step_reasoning_effort(-1) == 'medium')
assert(api.step_reasoning_effort(-1) == 'low')
api.set_profile('implement')
assert(info().reasoning_effort == 'medium')
local persisted = dofile(capstan.state_path('state.lua'))
assert(persisted.profile_models.implement.reasoning_effort == 'medium')
assert(persisted.profile_models.plan.reasoning_effort == 'low')

assert(capstan.models.set_profile('implement', 'openrouter', 'plain', 'default'))
assert(api.step_reasoning_effort(1) == nil)
assert(info().model == 'plain' and info().reasoning_effort == nil)
-- Explicit per-model metadata wins over supported_parameters=false.
runtime.providers.openrouter.reasoning_efforts = {plain = {'low', 'high'}}
assert(api.step_reasoning_effort(1) == 'low')
runtime.providers.openrouter.reasoning_efforts = nil

-- No catalog: use declared capabilities, never invent levels or fetch.
local provider = {endpoint = 'https://fixture.invalid/chat/completions'}
local fixture = {providers = {fixture = provider}}
assert(#models.cached_reasoning_efforts(fixture, 'fixture', 'm') == 0)
provider.default_reasoning_efforts = {'low', 'high'}
assert(#models.cached_reasoning_efforts(fixture, 'fixture', 'm') == 2)
-- A catalog explicitly fetched by /models also feeds the local hotkey cache.
http.get = function() return 200, '{"data":[{"id":"m","supported_parameters":[]}]}' end
assert(models.list(fixture, 'fixture'))
http.get = function() error('Hotkey must use cached catalog') end
assert(#models.cached_reasoning_efforts(fixture, 'fixture', 'm') == 0)

-- Launch overrides retain routing; manual effort changes remain profile-local.
assert(api.configure_interactive({provider = 'openrouter', model = 'config/model', reasoning_effort = 'low'}))
assert(api.step_reasoning_effort(1) == 'medium')
assert(info().model == 'config/model' and info().reasoning_effort == 'medium')
api.set_profile('plan')
assert(info().reasoning_effort == 'low')
api.set_profile('implement')
assert(info().reasoning_effort == 'medium')

-- A pending request keeps its captured effort; only the next run changes.
local requests = {}
local original_stream = http.post_stream
http.post_stream = function(_, body)
    requests[#requests + 1] = require('vendor.rxi.json').decode(body)
    return #requests
end
runtime.providers.openrouter.context_limit = 4096
capstan.agent.run({messages = {{role = 'user', content = 'first'}}, tools = {}})
assert(requests[1].reasoning.effort == 'medium')
assert(api.step_reasoning_effort(1) == 'high')
capstan.agent.run({messages = {{role = 'user', content = 'second'}}, tools = {}})
assert(requests[1].reasoning.effort == 'medium')
assert(requests[2].reasoning.effort == 'high')
assert(api.step_reasoning_effort(-1) == 'medium')
assert(api.step_reasoning_effort(-1) == 'low')
assert(api.step_reasoning_effort(-1) == 'default')
capstan.agent.run({messages = {{role = 'user', content = 'default'}}, tools = {}})
assert(not requests[3].reasoning or not requests[3].reasoning.effort)
assert(info().reasoning_effort == nil)
http.post_stream = original_stream
http.get = original_get

-- State write failures are surfaced, not reported as a successful save.
capstan.state_ensure_dir = function() return false end
local value, err = api.step_reasoning_effort(1)
assert(value == nil and err:find('could not be saved', 1, true))
assert(info().reasoning_effort == 'low')
