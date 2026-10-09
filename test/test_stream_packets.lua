-- Offline regression coverage for multiplexed OpenAI/OpenRouter SSE packets.
package.path = './?.lua;./?/init.lua;' .. package.path
local function noop() end
local hook_chunks, hook_drop = {}, false
package.loaded['agent.hooks'] = {has = function() return true end,
    run = function(_, ctx)
        hook_chunks[#hook_chunks + 1] = ctx.chunk.type
        assert(ctx.chunk.type ~= 'batch', 'hooks must receive individual chunks')
        if hook_drop and ctx.chunk.type == 'text' then ctx.chunk = nil end
        return ctx
    end}
agent = {set_usage = noop, set_thinking = noop, set_activity = noop}
popup = {error = noop}
capstan = {config = {}, now_ms = function() return 10 end}
local stream = require('agent.stream')
local json = require('vendor.rxi.json')
local function packet(event) return 'data: ' .. json.encode(event) end
local combined = {
    id = 'gen-fixture', model = 'deepseek/deepseek-v4-pro', provider = 'Fixture upstream',
    choices = {{delta = {reasoning = 'think', content = 'answer',
        reasoning_details = {{type = 'reasoning.text', text = 'think', index = 0}},
        tool_calls = {{index = 0, id = 'call-1', ['function'] = {name = 'probe', arguments = '{}'}}}}}},
    usage = {prompt_tokens = 100, completion_tokens = 10,
        prompt_tokens_details = {cached_tokens = 80},
        completion_tokens_details = {reasoning_tokens = 5}},
}
local function collect(wire, fragment, provider)
    local final, visible = nil, ''
    local cb = stream.stream(provider or {}, function(result, done)
        if done then final = result else visible = visible .. result.content end
    end)
    if fragment then
        for i = 1, #wire do cb(wire:sub(i,i), false) end
    else cb(wire, false) end
    cb('', true)
    assert(final)
    return final, visible
end
for _, ending in ipairs({'\n\n', '', '\r\n\r\n'}) do
    for _, fragment in ipairs({false, true}) do
        local r, visible = collect(packet(combined) .. ending, fragment)
        assert(r.text == 'answer' and visible == 'answer')
        assert(r.reasoning == 'think' and r.reasoning_details[1].text == 'think')
        assert(#r.tool_calls == 1 and r.tool_calls[1].arguments == '{}')
        assert(r.metrics.usage.completion_tokens == 10 and r.metrics.usage.cached_tokens == 80)
        assert(r.metrics.response_id == 'gen-fixture' and r.metrics.upstream_provider == 'Fixture upstream')
        assert(r.metrics.response_model == combined.model)
    end
end
-- Metadata-only initial packets survive later packets without identities.
local r = collect(packet({id = 'gen-only', provider = 'Backend'}) .. '\n\n' ..
    packet({choices = {{delta = {content = 'ok'}}}}) .. '\n\n')
assert(r.text == 'ok' and r.metrics.response_id == 'gen-only' and r.metrics.upstream_provider == 'Backend')
assert(r.metrics.usage == nil)
-- Usage alone must never fabricate text/tool output or an upstream identity.
r = collect(packet({usage = {completion_tokens = 0}}) .. '\n\n')
assert(r.text == '' and r.tool_calls == nil and r.metrics.upstream_provider == nil)
assert(r.metrics.usage.cached_tokens == nil)
-- Legacy parser overrides and hook suppression retain their contracts.
r = collect('data: custom\n\n', false, {parse_sse_event = function()
    return {type = 'text', content = 'custom'}
end})
assert(r.text == 'custom')
hook_drop = true
r = collect(packet(combined) .. '\n\n')
assert(r.text == '' and #r.tool_calls == 1 and r.metrics.usage.completion_tokens == 10)
hook_drop = false
r = collect(packet({error = {message = 'upstream failed'}, provider = 'Failed backend'}) .. '\n\n')
assert(r.ok == false and r.error_category == 'provider' and r.metrics.upstream_provider == 'Failed backend')
for _, bad in ipairs({'data: null', 'data: 42', 'data: "bad"', 'data: {', 'data: [DONE]'}) do
    assert(stream.parse_sse_event(bad) == nil)
end
-- Routing is configured once and copied per request, with full replacement.
local config = require('agent.provider_config')
local providers = config.build().providers
local request = {}
config.apply_request_options(request, providers.openrouter)
assert(request.provider.sort == 'throughput')
request.provider.sort = 'price'
assert(providers.openrouter.routing.sort == 'throughput')
request = {}; config.apply_request_options(request, providers.deepseek); assert(request.provider == nil)
for _, disabled in ipairs({false, {}}) do
    request = {}; config.apply_request_options(request, {routing = disabled}); assert(request.provider == nil)
end
capstan.config.providers = {openrouter = {routing = {only = {'fixture'}, allow_fallbacks = false}}}
providers = config.build().providers
request = {}; config.apply_request_options(request, providers.openrouter)
assert(request.provider.sort == nil and request.provider.allow_fallbacks == false)
request.provider.only[1] = 'changed'
assert(providers.openrouter.routing.only[1] == 'fixture')
local metrics = require('agent.telemetry').measurements(r.metrics)
assert(metrics.upstream_provider == 'Failed backend')
print('stream packets and routing: passed')
