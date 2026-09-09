-- Standalone, offline: vendor/lua-5.5.0/src/lua test/test_runtime_timeline.lua
package.path = './?.lua;./?/init.lua;' .. package.path
local function noop() end
package.loaded['agent.logging'] = {runtime_log = noop, compact = tostring,
    safe_error = tostring, truncate = function(s) return s, false end,
    debug = noop, raw_logging_enabled = function() return false end}
package.loaded['agent.hooks'] = {run = function(_, ctx) return ctx end,
    install_config = noop, install_existing_plugins = noop, has = function() return false end}
package.loaded['agent.ui'] = {append = noop}
package.loaded['agent.mcp'] = {is_mcp_tool = function() return false end,
    collect_tools = function() return {} end}
local clock, records = 0, {}
capstan = {config = {}, now_ms = function() return clock end, telemetry = {
    start = function(name, parent, attrs)
        local record = {name = name, parent = parent, attrs = attrs}
        records[#records + 1] = record
        return record
    end,
    end_span = function(record, ok, cancelled, attrs)
        assert(not record.ended, 'duplicate finish')
        record.ended, record.ok, record.cancelled, record.final = true, ok, cancelled, attrs or {}
    end,
}}
agent = {append = noop, set_usage = noop, set_thinking = noop, set_info = noop}
popup = {error = noop}
local telemetry = require('agent.telemetry')
local tools = require('agent.tools')
local json = require('vendor.rxi.json')
local function find(operation, first)
    local matches = {}
    for i = first or 1, #records do
        if records[i].attrs.operation == operation then matches[#matches + 1] = records[i] end
    end
    return matches
end
local ran = 0
plugins = {{id = 'probe', command = '/probe', tool = {name = 'probe'},
    handler = function() ran = ran + 1; return 'ok' end}}
local available = tools.collect()
local function call(name, args, extra)
    local root = telemetry.start('agent.run', nil, {operation = 'agent'})
    local ctx = {telemetry_context = root, tools = available, silent_tools = true,
        update_status = false, permission_scope = {}}
    for k, v in pairs(extra or {}) do ctx[k] = v end
    local ok, err = pcall(tools.handle_tool_calls, {}, available,
        {{id = 'call', name = name, arguments = json.encode(args or {})}}, '', noop, ctx)
    telemetry.finish(root, ok, false)
    return ok, err
end
permit = {check = function() return 'ask' end, prompt = function()
    clock = clock + 37; return 'allow'
end}
assert(call('probe'))
local wait = find('permission_wait')[1]
assert(wait.ended and wait.ok and wait.final.duration_ms == 37)
assert(wait.final.purpose == 'allow' and wait.parent.attrs.operation == 'tool')
assert(ran == 1)
permit.prompt = function() clock = clock + 9; return 'deny' end
assert(call('probe'))
wait = find('permission_wait')[2]
assert(not wait.ok and wait.final.purpose == 'deny' and ran == 1)
assert(wait.final['error.category'] == nil)
permit.prompt = function() error('private exception') end
assert(not call('probe'))
wait = find('permission_wait')[3]
assert(wait.ended and not wait.ok and wait.final.purpose == 'error')
assert(wait.final['error.category'] == 'exception')
permit.check = function() return 'deny' end
assert(call('probe'))
local decision = find('permission_decision')[1]
assert(decision.attrs.purpose == 'deny' and ran == 1)
assert(not decision.parent.ok and decision.parent.final['error.category'] == 'permission')
assert(wait.parent.final['error.category'] == 'exception')
assert(find('permission_wait')[2].parent.final['error.category'] == 'permission')
assert(find('permission_wait')[1].parent.final['error.category'] == nil)
assert(call('probe', {}, {guard = {total_tool_calls = 1, max_tool_calls = 1}}))
assert(find('guard_stop')[1].parent.attrs.operation == 'tool' and ran == 1)

-- Queued children retain the owning tool, including retry and cancellation.
permit.check = function() return 'allow' end
local pending, attempts = {}, 0
capstan.agent = {run = function(opts, callbacks)
    attempts = attempts + 1
    pending[#pending + 1] = {opts = opts, callbacks = callbacks}
    return true
end}
http = {poll = function()
    clock = clock + 23
    local child = table.remove(pending, 1)
    child.callbacks.on_done({ok = attempts ~= 1,
        error = attempts == 1 and 'HTTP 503' or '', text = 'done'})
end}
assert(call('subagents', {max_concurrent = 1, tasks = {
    {id = 'one', task = 'one'}, {id = 'two', task = 'two'},
}}))
local queues, retries = find('subagent_queue'), find('retry')
assert(#queues == 2 and queues[1].final.duration_ms == 0)
assert(queues[2].final.duration_ms == 46 and queues[2].ok)
assert(#retries == 1 and retries[1].final.duration_ms == 0)
assert(queues[1].parent == retries[1].parent and retries[1].attrs.attempt == 2)
local first = #records + 1
local cancelled = false
http.poll = function() cancelled = true end
assert(call('subagents', {max_concurrent = 1, tasks = {
    {id = 'one', task = 'one'}, {id = 'two', task = 'two'},
}}, {is_cancelled = function() return cancelled end}))
queues = find('subagent_queue', first)
assert(queues[2].ended and queues[2].cancelled and not queues[2].ok)

-- Real SSE parsing: optional detail counters, measured zero, malformed/absent.
local stream = require('agent.stream')
local function usage(value)
    local result
    local callback = stream.stream({suppress_agent_state = true}, function(r, done)
        if done then result = r end
    end)
    callback('data: ' .. json.encode({usage = value}) .. '\n\n', false)
    callback('data: {"choices":[{"delta":{"content":"done"}}]}\n\n', false)
    callback(nil, true)
    return result.metrics.usage
end
local source = {prompt_tokens = 10, prompt_tokens_details = {cached_tokens = 0},
    completion_tokens_details = {reasoning_tokens = 4}}
local measured = usage(source)
assert(measured.cached_tokens == 0 and measured.reasoning_tokens == 4)
assert(source.cached_tokens == nil)
measured = usage({prompt_tokens = 1})
assert(measured.cached_tokens == nil and measured.reasoning_tokens == nil)
measured = usage({cached_tokens = -1, reasoning_tokens = 'private'})
assert(measured.cached_tokens == nil and measured.reasoning_tokens == nil)

-- Actual run loop, mocked provider/config and transport only.
package.loaded['agent.provider_config'] = {build = function() return {
    provider = 'fixture', providers = {fixture = {model = 'fixture',
        endpoint = 'https://fixture.invalid', context_limit = 4096}},
} end}
package.loaded['agent.models'] = {install_runtime_api = function() capstan.models = {} end,
    profile = noop, ensure_context_limit = function() return 4096 end}
package.loaded['agent.tasks'] = {refresh = noop}
local runtime = require('agent.runtime')
local callbacks = {}
http.post_stream = function(_, _, _, callback) callbacks[#callbacks + 1] = callback end
-- Exercise the actual scheduler and run loop together with interleaved child
-- responses: model/tool descendants must remain beneath their own child.
first = #records + 1
local poll_count = 0
http.poll = function()
    poll_count = poll_count + 1
    if poll_count == 1 then
        assert(#callbacks == 2, 'both children start before either completes')
        callbacks[2]('data: {"choices":[{"delta":{"content":"right"}}]}\n\n', false)
        callbacks[2](nil, true)
        callbacks[1]('data: ' .. json.encode({choices = {{delta = {
            tool_calls = {{index = 0, id = 'inner', type = 'function',
                ['function'] = {name = 'probe', arguments = '{}'}}},
        }}}}) .. '\n\n', false)
        callbacks[1](nil, true)
    else
        callbacks[3]('data: {"choices":[{"delta":{"content":"left"}}]}\n\n', false)
        callbacks[3](nil, true)
    end
end
assert(call('subagents', {max_concurrent = 2, tasks = {
    {id = 'left', task = 'left'}, {id = 'right', task = 'right'},
}}))
local children = {}
for i = first, #records do
    local record = records[i]
    if record.name == 'subagent' then
        children[#children + 1] = record
        assert(record.attrs.subagent_index == #children)
        assert(record.attrs.subagent_id == (#children == 1 and 'left' or 'right'))
        assert(record.attrs.attempt == 1 and record.parent.name == 'agent.tool')
        assert(record.ended and record.ok)
    end
end
assert(#children == 2 and children[1].parent == children[2].parent)
local inner_tools, child_models = 0, 0
for i = first, #records do
    local record = records[i]
    if record.name == 'agent.model' then
        assert(record.parent == children[1] or record.parent == children[2])
        child_models = child_models + 1
    elseif record.name == 'agent.tool' and record ~= children[1].parent then
        assert(record.parent == children[1])
        inner_tools = inner_tools + 1
    end
end
assert(child_models == 3 and inner_tools == 1)
callbacks = {}
first = #records + 1
local result
assert(runtime.run({tools = {}, update_status = false, update_usage = false}, {
    on_done = function(r) result = r end,
}))
callbacks[1](nil, true, 'Connection error: fixture')
assert(#callbacks == 2)
callbacks[2]('data: ' .. json.encode({usage = source}) .. '\n\n', false)
callbacks[2]('data: {"choices":[{"delta":{"content":"done"}}]}\n\n', false)
callbacks[2](nil, true)
assert(result.ok)
retries = find('retry', first)
assert(#retries == 1 and retries[1].ended and retries[1].final.duration_ms == 0)
assert(retries[1].parent.attrs.operation == 'agent')
local models = find('agent', first)
local found_usage = false
for _, record in ipairs(models) do
    if record.name == 'agent.model' and record.final.cached_tokens == 0 then
        assert(record.final.reasoning_tokens == 4)
        found_usage = true
    end
end
assert(found_usage)
-- Every terminal path closes its owned model exactly once. Late transport
-- callbacks must neither retry nor deliver a second completion.
local function run_case(options, observers)
    callbacks = {}
    first = #records + 1
    local completions, terminal = 0, nil
    observers = observers or {}
    observers.on_done = function(r) completions = completions + 1; terminal = r end
    options = options or {}
    options.tools, options.update_status, options.update_usage = {}, false, false
    local ok, err = pcall(runtime.run, options, observers)
    return function(expected_ok, expected_cancelled)
        assert(completions == 1 and terminal.ok == expected_ok)
        local roots, model_count = 0, 0
        for i = first, #records do
            local record = records[i]
            assert(record.ended, 'unclosed terminal span')
            if record.name == 'agent.run' then
                roots = roots + 1
                assert(record.ok == expected_ok)
                assert(record.cancelled == (expected_cancelled == true))
            elseif record.name == 'agent.model' then
                model_count = model_count + 1
            end
        end
        assert(roots == 1)
        return terminal, model_count
    end, ok, err
end
local check = run_case()
callbacks[1](nil, true, 'Connection error: fixture')
callbacks[2](nil, true, 'Connection error: fixture')
local _, model_count = check(false)
assert(model_count == 2 and #find('retry', first) == 1)
for i = first, #records do
    local r = records[i]
    assert(r.final['error.category'] == (r.name ~= 'operation' and 'transport' or nil))
end
callbacks[1](nil, true, 'Connection error: late')
callbacks[2](nil, true, 'Connection error: late')
check(false)
assert(#callbacks == 2)

-- Visible output prevents transient-error replay.
check = run_case()
callbacks[1]('data: {"choices":[{"delta":{"content":"partial"}}]}\n\n', false)
callbacks[1](nil, true, 'Connection error: fixture')
check(false)
assert(#callbacks == 1 and #find('retry', first) == 0)

cancelled = false
check = run_case({is_cancelled = function() return cancelled end})
cancelled = true
callbacks[1](nil, true)
check(false, true)
callbacks[1](nil, true, 'Connection error: late')
check(false, true)
assert(#callbacks == 1)

-- Empty-response recovery hits the same run guard as tool continuation.
check = run_case({max_turns = 1})
callbacks[1](nil, true)
local terminal = check(false)
assert(terminal.error:find('max agent turns exceeded', 1, true))
local stops = find('guard_stop', first)
assert(#stops == 1 and stops[1].parent.name == 'agent.run' and not stops[1].ok)

check = run_case({provider = 'missing'})
check(false)
assert(#callbacks == 0)
local succeeded, failure
check, succeeded, failure = run_case({}, {on_model_start = function()
    error('fixture observer failure')
end})
assert(succeeded) -- Observer failures are handled run failures, not exceptions.
check(false)
assert(#callbacks == 0)

-- Unexpected setup-hook exceptions propagate, but still settle the run.
local hooks = package.loaded['agent.hooks']
local original_run = hooks.run
hooks.run = function() error('fixture setup failure') end
check, succeeded, failure = run_case()
hooks.run = original_run
assert(not succeeded and tostring(failure):find('fixture setup failure', 1, true))
check(false)
assert(#callbacks == 0)

-- Root aggregates are snapshots of completed callbacks, not a recursive sum
-- of native spans. Exercise classification/clipping independently of clocks.
local original_handle = tools.handle_tool_calls
local function aggregate_case(observer, tool_name, permission)
    callbacks = {}
    local root_first = #records + 1
    local done, root_result = 0
    tools.handle_tool_calls = function(messages, available_tools, _, _, continue, ctx)
        -- A separate child run overlaps the parent tool wait. Its large model
        -- measurement must remain on the child, never inflate the parent.
        local child_result
        runtime.run({depth = 1, telemetry_parent = ctx.telemetry_context,
            tools = {}, update_status = false, update_usage = false}, {
            on_done = function(r) child_result = r end,
        })
        local child_callback = callbacks[#callbacks]
        clock = clock + 100
        child_callback('data: {"choices":[{"delta":{"content":"child"}}]}\n\n', false)
        child_callback(nil, true)
        assert(child_result.measurements.model_ms == 100)
        ctx.callbacks.on_tool_done({name = tool_name or 'subagents'}, 'done', true, 40, permission or 9)
        continue(messages, available_tools)
    end
    runtime.run({tools = {}, update_status = false, update_usage = false}, {
        on_tool_done = observer,
        on_done = function(r) done = done + 1; root_result = r end,
    })
    clock = clock + 7
    local ok, err = pcall(callbacks[1], 'data: ' .. json.encode({choices = {{delta = {
        tool_calls = {{index = 0, id = 'call', type = 'function',
            ['function'] = {name = 'subagents', arguments = '{}'}}},
    }}}}) .. '\n\n', false)
    assert(ok, err)
    ok, err = pcall(callbacks[1], nil, true)
    if not observer then
        assert(ok, err)
        clock = clock + 3
        callbacks[#callbacks]('data: {"choices":[{"delta":{"content":"done"}}]}\n\n', false)
        callbacks[#callbacks](nil, true)
    else
        assert(not ok and tostring(err):find('fixture tool observer', 1, true))
    end
    assert(done == 1)
    local root = records[root_first]
    for key, value in pairs(root_result.measurements) do assert(root.final[key] == value, key) end
    assert(root_result.measurements.request_count == (observer and 1 or 2))
    assert(root_result.measurements.model_ms == (observer and 7 or 10))
    assert(root_result.measurements.tool_count == 1)
    tools.handle_tool_calls = original_handle
    return root_result, root
end
local aggregate = aggregate_case()
assert(aggregate.measurements.tool_ms == 0)
assert(aggregate.measurements.permission_wait_ms == 9)
assert(aggregate.measurements.subagent_wait_ms == 31)
assert(aggregate.measurements.unattributed_ms == aggregate.duration_ms - 50)
assert(aggregate.measurements.overlap_ms == 0)
aggregate = aggregate_case(nil, 'probe', 80)
assert(aggregate.measurements.permission_wait_ms == 40 and aggregate.measurements.tool_ms == 0)
assert(aggregate.measurements.subagent_wait_ms == 0)
aggregate = aggregate_case(nil, 'subagents_extra', -5)
assert(aggregate.measurements.permission_wait_ms == 0 and aggregate.measurements.tool_ms == 40)
assert(aggregate.measurements.subagent_wait_ms == 0)
local failed_root
aggregate, failed_root = aggregate_case(function() error('fixture tool observer') end)
assert(not aggregate.ok and aggregate.duration_ms == nil)
assert(aggregate.measurements.unattributed_ms == nil and aggregate.measurements.overlap_ms == nil)
assert(failed_root.final.duration_ms == nil and aggregate.measurements.subagent_wait_ms == 31)

-- Model export failure counts the completed measurement, but never invents
-- completion measurements for a request interrupted during start or text.
check = run_case({}, {on_model_done = function() error('fixture model observer') end})
clock = clock + 11
callbacks[1](nil, true, 'Connection error: fixture')
terminal = check(false)
assert(terminal.measurements.request_count == 1 and terminal.measurements.model_ms == 11)
check = run_case({}, {on_model_start = function() error('fixture start observer') end})
terminal = check(false)
assert(terminal.measurements.request_count == 0 and terminal.measurements.model_ms == 0)
check = run_case({}, {on_text = function() error('fixture text observer') end})
assert(not pcall(callbacks[1], 'data: {"choices":[{"delta":{"content":"partial"}}]}\n\n', false))
terminal = check(false)
assert(terminal.duration_ms == nil and terminal.measurements.request_count == 0)
assert(records[first].final.request_count == 0 and records[first].final.duration_ms == nil)

-- Forced owner cancellation and shutdown retain completed child measurements
-- before native end_span; the interrupted retry has no invented duration.
do
local saved_agent = capstan.agent
local saved_telemetry, saved_runtime = package.loaded['agent.telemetry'], package.loaded['agent.runtime']
package.loaded['agent.telemetry'], package.loaded['agent.runtime'] = nil, nil
local telemetry = require('agent.telemetry')
local runtime = require('agent.runtime')
for _, shutdown in ipairs({false, true}) do
    callbacks = {}
    local owner = telemetry.start('agent.tool', nil, {operation = 'tool'})
    local child_index = #records + 1
    local completions, child_result = 0, nil
    runtime.run({depth = 1, telemetry_parent = owner, tools = {},
        update_status = false, update_usage = false}, {
        on_done = function(r) completions = completions + 1; child_result = r end,
    })
    clock = clock + 13
    callbacks[1](nil, true, 'Connection error: fixture')
    assert(#callbacks == 2)
    clock = clock + 5
    if shutdown then telemetry.shutdown() else telemetry.finish(owner, false, true) end
    local child = records[child_index]
    assert(child.ended and child.cancelled and not child.ok)
    assert(child.final.request_count == 1 and child.final.model_ms == 13)
    assert(child.final.duration_ms == nil and child.final.unattributed_ms == nil)
    assert(completions == 1 and child_result.cancelled)
    for key, value in pairs(child_result.measurements) do
        assert(child.final[key] == value, key)
    end
    callbacks[2](nil, true, 'Connection error: late')
    assert(completions == 1 and #callbacks == 2)
end
package.loaded['agent.telemetry'], package.loaded['agent.runtime'] = saved_telemetry, saved_runtime
capstan.agent = saved_agent
end
for _, record in ipairs(records) do assert(record.ended) end
print('runtime timeline tests passed')
