-- Offline fixtures only. Run: vendor/lua-5.5.0/src/lua test/test_openai_siwc_responses.lua
package.path = './?.lua;./?/init.lua;' .. package.path
package.loaded['agent.hooks'] = {has = function() return false end}
local function noop() end
agent = {set_usage = noop, set_thinking = noop, set_activity = noop}
popup = {error = noop}
capstan = {config = {}, now_ms = function() return 10 end}
local json = require('vendor.rxi.json')
local adapter = require('agent.openai_siwc_responses')
local function rejects(f, expected)
    local ok, err = pcall(f)
    assert(not ok and tostring(err):find(expected, 1, true), tostring(err))
end
local tools = {{type = 'function', ['function'] = {name = 'mcp.probe', description = 'Fixture',
    parameters = {type = 'object'}, strict = false}}}
local messages = {{role = 'system', content = 'Be precise'}, {role = 'user', content = 'Hello'},
    {role = 'assistant', content = 'Checking', tool_calls = {{id = 'call-old', type = 'function',
        ['function'] = {name = 'mcp.probe', arguments = '{}'}}}},
    {role = 'tool', tool_call_id = 'call-old', content = 'done'}, {role = 'user', content = 'Again'}}
local body, registry = adapter.build_request({model = 'fixture', messages = messages, tools = tools})
local name = body.tools[1].tools[1].name
assert(body.store == false and body.stream == true and #body.input == 6)
assert(body.input[1].role == 'developer' and body.input[6].content == 'Again')
assert(body.tools[1].type == 'namespace' and body.tools[1].name == 'capstan')
assert(registry[name] == 'mcp.probe' and name:match('^[a-zA-Z0-9_-]+$'))
assert(body.input[4].namespace == 'capstan' and body.input[4].name == name)
assert(body.input[5].call_id == 'call-old' and body.input[5].type == 'function_call_output')
assert(body.tools[1].tools[1].strict == false and body.include[1] == 'reasoning.encrypted_content')
body.tools[1].tools[1].parameters.type = 'changed'
assert(tools[1]['function'].parameters.type == 'object')
for field in ('background conversation max_output_tokens max_tool_calls metadata moderation multi_agent prompt '
    .. 'prompt_cache_retention safety_identifier temperature top_logprobs top_p truncation user previous_response_id '
    .. 'max_tokens routing stream_options reasoning'):gmatch('%S+') do
    rejects(function() adapter.build_request({model = 'fixture', messages = {}, [field] = true}) end, 'Unsupported SIWC')
end
rejects(function() adapter.build_request({model = 'fixture', messages = {}, tools = {{type = 'tool_search'}}}) end, 'Only local')
rejects(function() adapter.build_request({model = 'fixture', messages = {{role = 'user', content = {{type = 'audio'}}}}}) end, 'Only text')
rejects(function() adapter.build_request({model = 'fixture', messages = {messages[4]}}) end, 'Unmatched')
rejects(function() adapter.build_request({model = 'fixture', messages = {messages[3]}}) end, 'Missing historical')
rejects(function() adapter.build_request({model = 'fixture', messages = {{role = 'assistant',
    responses = {format = 'private-plugin', items = {}}}}}) end, 'Foreign')

local reasoning_opts = {effort='low'}
local reasoning_body = adapter.build_request({model='fixture',messages={},reasoning=reasoning_opts})
assert(reasoning_body.reasoning.effort=='low')
reasoning_opts.effort='high'
assert(reasoning_body.reasoning.effort=='low')
for _, value in ipairs({{effort=1}, {effort='invented'}, {exclude=true}, {max_tokens=100}, {summary=true}}) do
    rejects(function() adapter.build_request({model='fixture',messages={},reasoning=value}) end,
        'Unsupported SIWC reasoning')
end

local call = {type = 'function_call', id = 'fc_1', call_id = 'call-new', namespace = 'capstan',
    name = name, arguments = '{"path":"fixture"}', status = 'completed'}
local reason = {type = 'reasoning', id = 'rs_1', summary = {}, encrypted_content = 'fixture-opaque', status = 'completed'}
local completed = {type = 'response.completed', response = {id = 'resp_fixture', model = 'fixture', status = 'completed',
    output = {reason, call}, usage = {input_tokens = 20, output_tokens = 4, total_tokens = 24,
        input_tokens_details = {cached_tokens = 3}, output_tokens_details = {reasoning_tokens = 2}}}}
local events = {
    {type = 'response.output_text.delta', delta = 'Hello'},
    {type = 'response.output_item.added', output_index = 1,
        item = {type = 'function_call', id = 'fc_1', call_id = 'call-new', name = name, namespace = 'capstan', arguments = ''}},
    {type = 'response.function_call_arguments.delta', item_id = 'fc_1', delta = '{"path":'},
    {type = 'response.function_call_arguments.delta', item_id = 'fc_1', delta = '"fixture"}'},
    {type = 'response.function_call_arguments.done', item_id = 'fc_1', arguments = call.arguments},
    {type = 'response.function_call_arguments.done', item_id = 'fc_1', arguments = call.arguments},
    {type = 'response.output_item.done', item = call},
    {type = 'response.output_item.done', item = reason}, completed,
}
local function wire(list, ending)
    local out = {}; for _, event in ipairs(list) do out[#out + 1] = 'data: ' .. json.encode(event) end
    return table.concat(out, ending) .. ending
end
local function collect(data, fragment, transport_error, eof_packet)
    local final, visible, finals = nil, '', 0
    local callback = adapter.stream({suppress_agent_state = true}, registry, function(result, done)
        if done then final = result; finals = finals + 1
        else assert(result.tool_calls == nil); visible = visible .. result.content end
    end)
    if not eof_packet then
        if fragment then for i = 1, #data do callback(data:sub(i,i), false) end
        else callback(data, false) end
    end
    assert(final == nil, 'Never dispatch tools before clean EOF')
    callback(eof_packet and data or '', true, transport_error)
    callback('', true)
    assert(finals == 1)
    return final, visible
end
for _, ending in ipairs({'\n\n', '\r\n\r\n', '\r\r'}) do
    for _, fragment in ipairs({false, true}) do
        local result, visible = collect(wire(events, ending), fragment)
        assert(result.ok and visible == 'Hello' and result.text == 'Hello')
        assert(#result.tool_calls == 1 and result.tool_calls[1].arguments == call.arguments)
        assert(result.tool_calls[1].name == 'mcp.probe' and result.tool_calls[1].id == 'call-new')
        assert(result.metrics.usage.prompt_tokens == 20 and result.metrics.usage.completion_tokens == 4)
        assert(result.metrics.usage.cached_tokens == 3 and result.metrics.usage.reasoning_tokens == 2)
        assert(result.metrics.response_id == 'resp_fixture')
        assert(result.native_output.items[1].encrypted_content == 'fixture-opaque')
        local replay = adapter.build_request({model = 'fixture', tools = tools, messages = {
            {role = 'assistant', responses = result.native_output},
            {role = 'tool', tool_call_id = 'call-new', content = 'fixture output'}, {role = 'user', content = 'Continue'}}})
        assert(#replay.input == 4 and replay.input[1].type == 'reasoning' and replay.input[2].call_id == 'call-new')
    end
end
assert(collect(wire(events, '\n\n'), false, nil, true).ok)
-- Unterminated last SSE packet is processed by shared framing at EOF.
assert(collect(wire(events, '\n\n'):sub(1, -3), true).ok)
local prefix = {}; for i = 1, #events - 1 do prefix[i] = events[i] end
for _, suffix in ipairs({'', 'data: [DONE]\n\n', 'data: {\n\n'}) do
    local r = collect(wire(prefix, '\n\n') .. suffix, true)
    assert(r.ok == false and r.tool_calls == nil and r.native_output == nil)
end
for _, kind in ipairs({'response.failed', 'response.incomplete', 'error'}) do
    local e = {type = kind, code = 'fixture_error', message = 'failed', response = {
        error = {code = 'subscription_sharing_usage_limit_exceeded', message = 'fixture quota'},
        incomplete_details = {reason = 'fixture_incomplete'}}}
    local r = collect(wire(prefix, '\n\n') .. wire({e}, '\n\n'), true)
    assert(not r.ok and r.tool_calls == nil and r.native_output == nil and r.error_code)
    r = collect(wire(events, '\n\n') .. wire({e}, '\n\n'))
    assert(not r.ok and r.tool_calls == nil)
end
local r = collect(wire(events, '\n\n'), false, 'fixture disconnected')
assert(not r.ok and r.tool_calls == nil and r.error_category == 'transport')
r = collect(wire(events, '\n\n') .. wire({completed}, '\n\n'))
assert(not r.ok and not r.tool_calls)
local function bad_completion(item)
    return collect(wire(prefix, '\n\n') .. wire({{type = 'response.completed', response = {
        status = 'completed', output = {item}}}}, '\n\n'))
end
for _, change in ipairs({{namespace = 'foreign'}, {arguments = '{'}, {name = 'unknown'}, {status = 'incomplete'}}) do
    local item = {}; for k, v in pairs(call) do item[k] = v end
    for k, v in pairs(change) do item[k] = v end
    r = bad_completion(item); assert(not r.ok and not r.tool_calls)
end
r = bad_completion(reason) -- A complete-looking response cannot drop a pending call.
assert(not r.ok and not r.tool_calls)
r = collect('data: ' .. json.encode({type = 'response.output_text.delta', delta = 1}) .. '\n\n' .. wire({completed}, '\n\n'))
assert(not r.ok and not r.tool_calls)
-- Plain text success does not need tools or fabricated usage.
r = collect(wire({{type = 'response.output_text.delta', delta = 'ok'},
    {type = 'response.completed', response = {status = 'completed', output = {
        {type = 'message', role = 'assistant', id = 'msg_fixture', status = 'completed',
            content = {{type = 'output_text', text = 'ok', annotations = {}}}}}}}}, '\n\n'))
assert(r.ok and r.text == 'ok' and r.tool_calls == nil and r.metrics.usage == nil)
-- Second-turn wire JSON must retain array types, including restored legacy {} fields.
for _, annotations in ipairs({false, {}, json.array({}), {{type='url_citation',
    start_index=0, end_index=2, title='Fixture', url='https://example.invalid'}}}) do
    local text = {type='output_text', text='ok'}
    if annotations then text.annotations = annotations end
    local item = {type='message', id='second-turn', role='assistant', status='completed', content={text}}
    local result = collect(wire({{type='response.completed', response={status='completed', output={
        {type='reasoning', id='second-reason', summary={}, content={}, encrypted_content='fixture'}, item}}}}, '\n\n'))
    assert(result.ok)
    for _, native in ipairs({result.native_output, json.decode(json.encode(result.native_output))}) do
        local replay = adapter.build_request({model='fixture', messages={
            {role='user', content='Hello'}, {role='assistant', responses=native},
            {role='user', content='Again'}}})
        local encoded = json.encode(replay)
        assert(encoded:find('"summary":[]', 1, true) and encoded:find('"content":[]', 1, true))
        local actual = json.decode(encoded).input[3].content[1].annotations
        assert(#actual == (annotations and #annotations or 0))
        if #actual == 0 then
            assert(json.encode(actual) == '[]')
        else
            for key, value in pairs(annotations[1]) do assert(actual[1][key] == value) end
            replay.input[3].content[1].annotations[1].title = 'changed'
            assert(native.items[2].content[1].annotations[1].title == 'Fixture')
        end
    end
end
for _, annotations in ipairs({true, 'invalid', {type='url_citation'}, {'invalid'}}) do
    rejects(function() adapter.build_request({model='fixture', messages={{role='assistant', responses={
        format=adapter.history_format, tool_names={}, items={{type='message', role='assistant',
            content={{type='output_text', text='ok', annotations=annotations}}}}}}}}) end, 'Invalid output annotation')
end
assert(json.encode(adapter.build_request({model='fixture', messages={}})):find('"input":[]', 1, true))
-- Native message phases survive done/completed snapshots, JSON persistence and replay.
for _, phase in ipairs({'commentary', 'final_answer', false}) do
    local item = {type='message', id='phase-message', role='assistant', status='completed',
        content={{type='output_text', text='fixture'}}}
    if phase then item.phase = phase end
    local result = collect(wire({{type='response.output_item.done', item=item},
        {type='response.completed', response={status='completed', output={item}}}}, '\n\n'), true)
    assert(result.ok and result.native_output.items[1].phase == (phase or nil))
    local persisted = json.decode(json.encode(result.native_output))
    local replay = adapter.build_request({model='fixture', messages={{role='assistant', responses=persisted}}})
    assert(replay.input[1].phase == (phase or nil))
    if phase then
        item.phase = phase == 'commentary' and 'final_answer' or 'commentary'
        local changed = collect(wire({{type='response.output_item.done', item=persisted.items[1]},
            {type='response.completed', response={status='completed', output={item}}}}, '\n\n'))
        assert(not changed.ok and not changed.native_output)
    end
    for _, invalid in ipairs({true, 42, 'invented'}) do
        item.phase = invalid
        local bad = collect(wire({{type='response.completed', response={status='completed', output={item}}}}, '\n\n'))
        assert(not bad.ok and not bad.native_output)
        persisted.items[1].phase = invalid
        rejects(function() adapter.build_request({model='fixture', messages={{role='assistant', responses=persisted}}}) end,
            'Invalid assistant phase')
    end
end
local function clone(value) return json.decode(json.encode(value)) end
local function fails_closed(list)
    local result = collect(wire(list, '\n\n'), true)
    assert(result.ok == false and result.tool_calls == nil and result.native_output == nil)
end
-- Every observed argument snapshot must agree, including overwritten done events.
for _, index in ipairs({3, 5, 6, 7}) do
    local list = clone(events)
    if index == 3 then list[index].delta = '{"wrong":'
    elseif index == 7 then list[index].item.arguments = '{"wrong":true}'
    else list[index].arguments = '{"wrong":true}' end
    fails_closed(list)
end
local list = clone(events)
list[2].item.arguments = '{"wrong":'
fails_closed(list)
list = clone(events)
list[2].item.call_id = 'changed'
fails_closed(list)
list = clone(events)
list[2].item.name = 'changed'
fails_closed(list)
list = clone(events)
table.insert(list, 7, {type = 'response.function_call_arguments.delta', item_id = 'fc_1', delta = ' '})
fails_closed(list)
list = clone(events)
local changed = clone(call); changed.arguments = '{"wrong":true}'
table.insert(list, 8, {type = 'response.output_item.done', item = changed})
fails_closed(list)
list = clone(events)
table.insert(list, 3, clone(list[2]))
fails_closed(list)
list = clone(events)
local duplicate = clone(call); duplicate.id = 'fc_duplicate'
table.insert(list[#list].response.output, duplicate)
fails_closed(list)
list = clone(events)
list[#list].response.output[2] = {type = 'reasoning', id = 'fc_1', summary = {}}
fails_closed(list)
-- A final snapshot may complete a streamed prefix, but cannot replace it.
assert(collect(wire({events[2], events[3], completed}, '\n\n'), true).ok)
assert(collect(wire({completed}, '\n\n'), true).ok)
list = clone(events)
list[2].item.arguments = list[3].delta
table.remove(list, 3)
assert(collect(wire(list, '\n\n'), true).ok)
-- Aliases survive tool removal, reordering, and long/unusual original names.
local long_name = 'mcp.' .. string.rep('long-name/', 30)
local long_tool = {type = 'function', ['function'] = {name = long_name}}
local long_body, long_registry = adapter.build_request({model = 'fixture', messages = {}, tools = {long_tool}})
local long_alias = long_body.tools[1].tools[1].name
assert(#long_alias <= 128 and long_registry[long_alias] == long_name)
local saved = collect(wire(events, '\n\n')).native_output
assert(saved.tool_names[name] == 'mcp.probe')
local history = {{role = 'assistant', responses = saved},
    {role = 'tool', tool_call_id = 'call-new', content = 'done'}}
local replay, replay_registry = adapter.build_request({model = 'fixture', messages = history, tools = {long_tool}})
assert(replay.input[2].name == name and replay_registry[name] == nil)
assert(replay_registry.tool_names[name] == 'mcp.probe')
assert(replay.tools[1].tools[1].name ~= name)
local reordered = adapter.build_request({model = 'fixture', messages = history, tools = {long_tool, tools[1]}})
assert(reordered.tools[1].tools[2].name == name)
local old = clone(messages)
old[3].tool_calls[1]['function'].name = long_name
local old_body = adapter.build_request({model = 'fixture', messages = old, tools = {tools[1], long_tool}})
assert(old_body.input[4].name == old_body.tools[1].tools[2].name)
local invalid = clone(history)
invalid[1].responses.tool_names = nil
rejects(function() adapter.build_request({model = 'fixture', messages = invalid}) end, 'requires tool_names')
invalid = clone(history)
invalid[1].responses.tool_names = {}
rejects(function() adapter.build_request({model = 'fixture', messages = invalid}) end, 'Unknown historical')
invalid = clone(history)
local conflicting = clone(saved); conflicting.tool_names[name] = 'different'
table.insert(invalid, {role = 'assistant', responses = conflicting})
rejects(function() adapter.build_request({model = 'fixture', messages = invalid}) end, 'Conflicting historical')
print('openai_siwc_responses: passed (offline request/history and fragmented terminal-gated stream fixtures)')
