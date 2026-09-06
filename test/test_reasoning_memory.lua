-- Runs in the isolated provider/state fixture from test_provider_tools.c.
capstan.config.agent = {profile = 'implement', reasoning_effort = 'high'}
local catalog = {
    {id = 'a', supported_parameters = {'reasoning'}},
    {id = 'b', supported_parameters = {'reasoning'}},
    {id = 'new', supported_parameters = {'reasoning'}},
    {id = 'plain', supported_parameters = {}},
}
-- /models lists all providers; keep even the unused provider offline.
capstan.config.providers.deepseek = {models = {}}
capstan.config.providers.openrouter.models = catalog
capstan.config.providers.openrouter.default_reasoning_efforts = {'low', 'medium', 'high'}
capstan.config.providers.other = {
    model = 'a', models = catalog, default_reasoning_efforts = {'low', 'medium', 'high'},
    endpoint = 'https://fixture.invalid/chat/completions', context_limit = 4096,
}
-- Migrate the old single-selection format when first leaving that model.
capstan.state = {profile_models = {
    implement = {provider = 'openrouter', model = 'a', reasoning_effort = 'high'},
}}
local runtime = dofile('agent/runtime.lua')
local api, models = capstan.agent, capstan.models
local function effort()
    return capstan.models.effective().reasoning_effort or 'default'
end
local original_get = http.get
http.get = function() error('Restoring effort must not fetch a catalog') end
assert(effort() == 'high')
assert(models.set_profile('implement', 'openrouter', 'b'))
assert(effort() == 'default') -- not config high, not model A's high
assert(api.step_reasoning_effort(1) == 'low')
assert(api.step_reasoning_effort(1) == 'medium')
assert(models.set_profile('implement', 'openrouter', 'a'))
assert(effort() == 'high')
assert(models.set_profile('implement', 'other', 'a'))
assert(effort() == 'default')
assert(api.step_reasoning_effort(1) == 'low')
assert(models.set_profile('implement', 'openrouter', 'a'))
assert(effort() == 'high')
assert(models.set_profile('plan', 'openrouter', 'a', 'medium'))
api.set_profile('plan')
assert(effort() == 'medium')
api.set_profile('implement')
assert(effort() == 'high')

-- The model popup bypasses the effort drill only for remembered tuples.
local plugin = dofile('plugins/models.lua')
local entries = plugin.autocomplete.fetch({})
local seen = {}
for _, entry in ipairs(entries) do
    if entry.value == 'profile\timplement\topenrouter\ta' then seen.saved = true end
    if entry.value:find('efforts\tprofile\timplement\topenrouter\tnew\t', 1, true) == 1 then
        seen.new = true
    end
end
assert(seen.saved and seen.new)
local function command(args)
    return plugin.handler({args = args, replace = function(_, text) return text end})
end
assert(command({'--profile', 'implement', 'openrouter', 'b'}):find('reasoning: medium', 1, true))
assert(effort() == 'medium')
assert(command({'--profile', 'implement', 'openrouter', 'b', 'default'}):find('reasoning: default', 1, true))
assert(models.set_profile('implement', 'openrouter', 'a'))
assert(models.set_profile('implement', 'openrouter', 'b'))
assert(effort() == 'default') -- an explicit reset is remembered, not forgotten
assert(models.set_profile('implement', 'openrouter', 'b', 'medium'))

-- Simulate restart from the actual serialized state, not the in-memory table.
capstan.state = dofile(capstan.state_path('state.lua'))
local history = capstan.state.profile_reasoning_efforts
assert(history.implement.openrouter.a == 'high')
assert(history.implement.openrouter.b == 'medium')
assert(history.implement.other.a == 'low')
assert(history.plan.openrouter.a == 'medium')
runtime = dofile('agent/runtime.lua')
api, models = capstan.agent, capstan.models
assert(effort() == 'medium')
assert(models.set_profile('implement', 'openrouter', 'a'))
assert(effort() == 'high')
assert(models.set_profile('implement', 'other', 'a'))
assert(effort() == 'low')
api.set_profile('plan')
assert(effort() == 'medium')
api.set_profile('implement')

-- Changed capabilities: default on restore, with no network or invented levels.
runtime.providers.openrouter.reasoning_efforts = {a = {'low'}}
assert(models.set_profile('implement', 'openrouter', 'a'))
assert(effort() == 'default')
assert(api.reasoning_effort('implement') == nil)
assert(api.step_reasoning_effort(1) == 'low')
assert(models.set_profile('implement', 'openrouter', 'plain'))
assert(effort() == 'default')
assert(api.step_reasoning_effort(1) == nil)
assert(models.set_profile('implement', 'openrouter', 'a'))
assert(effort() == 'low')

-- Requests and status agree; explicit run effort still takes precedence.
local requests = {}
local original_stream = http.post_stream
http.post_stream = function(_, body)
    requests[#requests + 1] = require('vendor.rxi.json').decode(body)
    return #requests
end
-- Avoid context discovery: it is independent of effort restoration.
require('agent.models').ensure_context_limit = function() return 4096 end
api.run({messages = {{role = 'user', content = 'test'}}, tools = {}})
assert(requests[1].reasoning.effort == 'low')
api.run({messages = {{role = 'user', content = 'explicit'}}, tools = {}, reasoning_effort = 'high'})
assert(requests[2].reasoning.effort == 'high')
runtime.providers.openrouter.reasoning_effort = 'high'
runtime.providers.openrouter.reasoning = {effort = 'high', exclude = true}
assert(models.set_profile('implement', 'openrouter', 'b', 'default'))
api.run({messages = {{role = 'user', content = 'default'}}, tools = {}})
assert(requests[3].reasoning.exclude == true and not requests[3].reasoning.effort)
http.post_stream = original_stream
http.get = original_get

-- Save failure keeps both the active selection and history in memory.
capstan.state_ensure_dir = function() return false end
local value, err = api.step_reasoning_effort(1)
assert(not value and err:find('could not be saved', 1, true))
assert(effort() == 'low')
assert(capstan.state.profile_reasoning_efforts.implement.openrouter.b == 'low')
assert(not models.set_profile('implement', 'other', 'a'))
assert(effort() == 'low')
assert(not models.set_profile('implement', 'openrouter', 'b'))
assert(effort() == 'low')
