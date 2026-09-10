package.path = './?.lua;' .. package.path
local models = require('agent.models')
local requests = 0
http = {get = function()
    requests = requests + 1
    return 200, '{"data":[{"id":"remote","context_length":8192}]}'
end}
local p = {model = 'a', configured_context_limit = 16384, context_limit = 999,
    models = {{id = 'a', context_limit = 32768, context_length = 123,
        top_provider = {context_length = 456}}, {id = 'b', context_limit = 65536}}}
assert(models.ensure_context_limit(p) == 32768)
p.model = 'b'; p.context_limit = 0
assert(models.ensure_context_limit(p) == 65536)
p.model = 'other'; p.context_limit = 0
assert(models.ensure_context_limit(p) == 16384)
assert(requests == 0)
local copy = {}
for k, v in pairs(p) do copy[k] = v end
copy.model = 'a'; copy.context_limit = 0
assert(models.ensure_context_limit(copy) == 32768)
assert(p.context_limit == 16384)
local remote = {model = 'remote',
    models_endpoint = 'https://fixture.invalid/models'}
assert(models.ensure_context_limit(remote) == 8192)
assert(requests == 1)
assert(models.ensure_context_limit(remote) == 8192)
assert(requests == 1)
assert(models.ensure_context_limit({model = 'unknown'}) == 0)
print('Context limit precedence tests passed')
