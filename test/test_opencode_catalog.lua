-- Public Go schema fixtures only; no real network, environment or credentials.
package.path = './?.lua;' .. package.path
local json = require('vendor.rxi.json')
local clock, requests = 1000, 0
os.time = function() return clock end
os.getenv = function() return nil end
capstan = {version = 'fixture', config = {}}
local provider = require('agent.provider_config').build().providers.opencode_go
assert(provider.models == nil and provider.catalog_format == 'opencode_go')
assert(provider.models_endpoint == 'https://opencode.ai/inference/go/v1/models')
provider.api_key = 'fixture-must-not-be-sent'
local runtime = {provider = 'opencode_go', providers = {opencode_go = provider}}
local rows = {
    {id='deepseek-v4-pro', name='DeepSeek V4 Pro', type='chat', go={},
        modalities={output={'text'}}, limit={context=1000000},
        capabilities={reasoning={supported=true, effort={'high','max','future','high'}}},
        api={['chat-completions']={base_url='https://wrong.invalid'}}},
    {id='claude-fixture', name='Claude fixture', type='chat', go={},
        modalities={output={'text'}}, limit={context=200000},
        capabilities={reasoning={supported=true, effort={'low','medium','high'}}},
        api={messages={base_url='https://opencode.ai/inference/go/anthropic/v1'}}},
    {id='unknown-limits', type='chat', go={}, modalities={output={'text'}},
        limit={context='invented'}, capabilities={reasoning={supported=false, effort={'high'}}}},
    {id='zen-only', type='chat', modalities={output={'text'}}},
    {id='image-only', type='image', go={}, modalities={output={'image'}}},
    {id='deepseek-v4-pro', type='chat', go={}, modalities={output={'text'}}},
    {id=42, type='chat', go={}, modalities={output={'text'}}},
    false,
}
local status, body = 200, json.encode({object='list', data=rows})
http = {get = function(url, headers, options)
    requests = requests + 1
    assert(url == provider.models_endpoint)
    assert(headers.Authorization == nil and headers['x-opencode-session'] == nil)
    assert(headers['User-Agent'] == 'capstan/fixture')
    assert(options.https_only and options.follow_redirects == false)
    return status, body
end}
local models = require('agent.models')
local list, err = models.list(runtime, 'opencode_go')
assert(not err and #list == 3 and requests == 1)
assert(list[1].id == 'claude-fixture' and list[2].id == 'deepseek-v4-pro')
assert(list[2].text == 'deepseek-v4-pro  DeepSeek V4 Pro')
assert(list[2].context_limit == 1000000)
assert(table.concat(list[2].reasoning_efforts, ',') == 'high,max')
assert(list[3].context_limit == 0 and list[3].reasoning_efforts == nil)
assert(provider.endpoint == 'https://opencode.ai/inference/go/openai/v1/chat/completions')
assert(models.ensure_context_limit(provider) == 1000000 and requests == 1)
assert(table.concat(models.cached_reasoning_efforts(runtime,'opencode_go','claude-fixture'), ',') == 'low,medium,high')
models.install_runtime_api(runtime)
local plugin = dofile('plugins/models.lua')
local picker = plugin.autocomplete.fetch({})
assert(#picker == 3 and picker[1].text:find('opencode_go/claude-fixture',1,true))
assert(picker[1].value:find('efforts',1,true))
assert(requests == 1)
-- Manual configuration still wins, without writing remote metadata into config.
provider.reasoning_efforts = {['deepseek-v4-pro']={'low'}}
assert(models.list(runtime,'opencode_go')[2].reasoning_efforts[1] == 'low')
provider.reasoning_efforts = nil
provider.models = {{id='manual', context_limit=1234, reasoning_efforts={'medium'}}}
assert(models.list(runtime,'opencode_go')[1].id == 'manual' and requests == 1)
provider.models = nil
provider.configured_context_limit = 4321; provider.context_limit = 0
assert(models.ensure_context_limit(provider) == 4321 and requests == 1)
-- Expiry refreshes, and failures preserve the previous public list with a warning.
clock = clock + 301; status = 503
list, err = models.list(runtime,'opencode_go')
assert(#list == 3 and err:find('cached',1,true))
picker = plugin.autocomplete.fetch({})
assert(#picker == 4 and picker[1].value == '' and picker[1].text:find('HTTP 503',1,true))
status, body = 200, '{broken'
list, err = models.list(runtime,'opencode_go')
assert(#list == 3 and err:find('invalid response',1,true))
rows[#rows+1] = {id='new-go', type='chat', go={}, modalities={output={'text'}}, limit={context=8192}}
body = json.encode({data=rows})
list, err = models.list(runtime,'opencode_go')
assert(not err and #list == 4)
-- Cold failure is visible, never a fabricated single-model catalog; retry recovers.
provider.models_endpoint = 'https://fixture.invalid/models'
status, body = 403, 'fixture-must-not-be-sent'
list, err = models.list(runtime,'opencode_go')
assert(list == nil and err:find('HTTP 403',1,true) and not err:find(body,1,true))
picker = plugin.autocomplete.fetch({})
assert(#picker == 1 and picker[1].value == '')
status, body = 200, '{"data":[]}'
list, err = models.list(runtime,'opencode_go')
assert(list == nil and err:find('no text models',1,true))
body = json.encode({data=rows})
list, err = models.list(runtime,'opencode_go')
assert(#list == 4 and not err)
assert(provider.model == 'deepseek-v4-pro' and provider.models == nil)
print('OpenCode Go catalog: metadata, picker, cache, failures and isolation passed')
