-- Offline genuine runtime + SSE, mocked transport/native workspace adapter.
package.path = './?.lua;./?/init.lua;' .. package.path
local function noop() end
package.loaded['agent.logging'] = {runtime_log = noop, compact = tostring, safe_error = tostring,
    raw_logging_enabled = function() return false end, debug = noop}
package.loaded['agent.hooks'] = {has = function() return false end, run = function(_, ctx) return ctx end,
    install_config = noop, install_existing_plugins = noop}
package.loaded['agent.mcp'] = {}
package.loaded['agent.ui'] = {append = function() error('background UI write') end}
package.loaded['agent.tasks'] = {refresh = noop}
package.loaded['agent.provider_config'] = {build = function() return {
    provider = 'fixture', providers = {fixture = {model = 'original',
        endpoint = 'https://fixture.invalid', context_limit = 4096}},
} end}
package.loaded['agent.models'] = {install_runtime_api = function() capstan.models = {} end,
    profile = noop, ensure_context_limit = noop}
local tool_context, inside_tool
package.loaded['agent.tools'] = {collect = function() return {} end, names = function() return '' end,
    handle_tool_calls = function(messages, available, _, _, continue, ctx)
        tool_context = ctx
        if inside_tool then inside_tool() end
        continue(messages, available)
    end}
capstan = {config = {}, workdir = '/launch', workspace_root = '/launch'}
system_prompt = 'launch prompt'
agent = {set_info = noop, set_usage = noop, set_profile_info = noop, session_id = function() return 'owner' end}
local native_dir, native_root = '/launch', '/launch'
local native_cwd, native_explicit, native_owner = '/launch', false, 'ambient-owner'
local stopped = {}
tools = {background_context = function(dir, root, cwd, explicit)
    local d, r, c, e = native_dir, native_root, native_cwd, native_explicit
    native_dir, native_root = dir, root
    native_cwd = cwd or dir
    native_explicit = explicit == nil and true or explicit
    return d, r, c, e
end, process_scope = function(owner)
    local previous = native_owner
    native_owner = owner
    return previous
end, background_cancelled = function(id) return stopped[id] end}
local requests, cancelled = {}, {}
local json = require('vendor.rxi.json')
http = {post_stream = function(_, body, _, callback, _, options)
    requests[#requests + 1] = {body = json.decode(body), callback = callback, options = options}
    return #requests
end, cancel = function(id) cancelled[id] = true end}
local runtime = require('agent.runtime')
local telemetry = require('agent.telemetry')
local parent = telemetry.start('agent.tool')
local done, texts = 0, 0
local ok, err, cancel = runtime.run({background = true, depth = 1,
    telemetry_parent = parent, background_id = 'child', profile = 'implement', tools = {}}, {
    on_text = function() texts = texts + 1 end,
    on_done = function(result)
        assert(capstan.workdir == '/launch' and native_dir == '/launch')
        done = done + 1
    end,
})
assert(ok and err == nil and type(cancel) == 'function')
assert(requests[1].options.background)
telemetry.finish(parent, true, false)
assert(done == 0, 'detached child survives launching tool completion')
capstan.workdir, capstan.workspace_root = '/other', '/other'
native_dir, native_root = '/other', '/other'
system_prompt = 'changed prompt'
capstan.agent.set_profile('plan')
runtime.providers.fixture.model = 'changed'
agent.set_info = function() error('background status write') end
agent.set_usage = agent.set_info
local function delta(index, value)
    requests[index].callback('data: ' .. json.encode({choices = {{delta = value}}}) .. '\n\n', false)
end
inside_tool = function()
    assert(capstan.workdir == '/launch' and native_dir == '/launch')
    assert(capstan.workspace_root == '/launch' and native_root == '/launch')
    agent_background_poll() -- Must not recursively schedule while inside tools.
end
delta(1, {tool_calls = {{index = 0, id = 'call', type = 'function',
    ['function'] = {name = 'probe', arguments = '{}'}}}})
requests[1].callback(nil, true)
assert(tool_context.profile == 'implement' and tool_context.process_owner == 'owner')
assert(tool_context.system_prompt == 'launch prompt' and tool_context.workdir == '/launch')
assert(requests[2].body.model == 'original')
assert(requests[2].body.messages[1].content:find('launch prompt', 1, true))
assert(capstan.workdir == '/other' and native_dir == '/other')
-- Cancellation does not require an HTTP reply, and late chunks are inert.
stopped.child = true
agent_background_poll()
assert(done == 1 and cancelled[2])
delta(2, {content = 'late'})
requests[2].callback(nil, true)
cancel()
assert(done == 1 and texts == 0)
assert(capstan.workdir == '/other' and native_dir == '/other')
-- Cross-run callbacks are deferred until the captured tool context unwinds.
local second_done = false
local _, _, cancel_second = runtime.run({background = true, tools = {}, workdir = '/second',
    workspace_root = '/second'}, {on_done = function() second_done = true end})
local second = #requests
runtime.run({background = true, tools = {}, workdir = '/first', workspace_root = '/first'}, {})
local first = #requests
inside_tool = function()
    delta(second, {content = 'second'})
    requests[second].callback(nil, true)
    assert(not second_done and native_dir == '/first')
end
delta(first, {tool_calls = {{index = 0, id = 'nested', type = 'function',
    ['function'] = {name = 'probe', arguments = '{}'}}}})
requests[first].callback(nil, true)
assert(not second_done)
agent_background_poll()
assert(second_done and native_dir == '/other')
cancel_second()
-- A foreground processes-wait tool explicitly yields to child tools. Ordinary
-- modal polling must not do so, even inside a child reached through that yield.
runtime.shutdown_background()
local poll_action
http.background_wait_safe = function() return true end
http.poll = function() if poll_action then poll_action() end end
local function tool_reply(index)
    delta(index, {tool_calls = {{index = 0, id = 'probe-' .. index, type = 'function',
        ['function'] = {name = 'probe', arguments = '{}'}}}})
    requests[index].callback(nil, true)
end
local child_done, modal_done, child_tools, modal_tools = false, false, 0, 0
runtime.run({background = true, tools = {}, workdir = '/wait-child',
    workspace_root = '/child-root', process_owner = 'child-owner'}, {
    on_done = function(result) assert(result.ok); child_done = true end,
})
local child_request = #requests
runtime.run({background = true, tools = {}, workdir = '/modal',
    process_owner = 'modal-owner'}, {on_done = function() modal_done = true end})
local modal_request = #requests
native_dir, native_root, native_cwd, native_explicit = '/other', '/other', '/physical-parent', false
local function assert_parent()
    assert(capstan.workdir == '/other' and capstan.workspace_root == '/other')
    assert(native_dir == '/other' and native_root == '/other')
    assert(native_cwd == '/physical-parent' and native_explicit == false,
        'wait must restore parent native cwd and explicit-workdir flag')
    assert(native_owner == 'parent-owner', 'wait must restore parent process owner')
end
inside_tool = function()
    if tool_context.process_owner == 'parent-owner' then
        assert_parent()
        tool_reply(child_request)
        _G.agent_background_poll()
        assert(child_tools == 0 and not child_done, 'modal poll entered background tools')
        poll_action = function()
            poll_action = nil
            delta(#requests, {content = 'child complete'})
            requests[#requests].callback(nil, true)
        end
        _G.agent_background_wait_poll()
        assert(child_tools == 1 and child_done, 'explicit wait did not complete child')
        assert_parent()
    elseif tool_context.process_owner == 'child-owner' then
        child_tools = child_tools + 1
        assert(native_dir == '/wait-child' and native_root == '/child-root')
        assert(native_owner == 'child-owner' and capstan.workdir == '/wait-child')
        tool_reply(modal_request)
        _G.agent_background_poll()
        assert(modal_tools == 0 and not modal_done, 'nested modal poll ran background tools')
    elseif tool_context.process_owner == 'modal-owner' then
        modal_tools = modal_tools + 1
    end
end
runtime.run({tools = {}, process_owner = 'parent-owner', update_status = false,
    update_usage = false, completion_review = false}, {on_text = noop})
tool_reply(#requests)
assert(native_owner == 'ambient-owner')
assert(child_done and modal_tools == 1)
runtime.shutdown_background()

-- Queued launches pass the captured profile as opts.profile (a valid named
-- table), and the captured provider as provider_snapshot, not live config.
local provider_snapshot = {model = 'queued-model', endpoint = 'https://queued.invalid',
    context_limit = 4096, reasoning = {effort = 'low'}}
local profile_snapshot = {name = 'implement', prompt = 'queued profile prompt'}
runtime.providers.fixture.model = 'live-model'
local queued_ok = runtime.run({background = true, tools = {}, provider = 'fixture',
    provider_snapshot = provider_snapshot, profile = profile_snapshot,
    workdir = '/queued', workspace_root = '/queued-root', system_prompt = 'queued system'}, {})
assert(queued_ok, 'valid named profile snapshot was rejected')
local queued_request = #requests
provider_snapshot.model, provider_snapshot.reasoning.effort = 'mutated-model', 'high'
profile_snapshot.prompt = 'mutated profile prompt'
inside_tool = function()
    assert(tool_context.profile == 'implement')
    assert(tool_context.profile_snapshot.prompt == 'queued profile prompt')
    assert(tool_context.provider.model == 'queued-model')
    assert(tool_context.provider.reasoning.effort == 'low')
    assert(native_dir == '/queued' and capstan.workspace_root == '/queued-root')
end
tool_reply(queued_request)
assert(requests[#requests].body.model == 'queued-model')
assert(requests[#requests].body.messages[1].content:find('queued profile prompt', 1, true))
assert(not requests[#requests].body.messages[1].content:find('mutated profile prompt', 1, true))
assert(capstan.workdir == '/other' and native_owner == 'ambient-owner')
runtime.shutdown_background()

-- Both byte and callback-count caps discard retained SSE, cancel transport,
-- and report one terminal error without executing buffered tools or late data.
for _, limit in ipairs({'bytes', 'callbacks'}) do
    local result, completions, output, executed = nil, 0, 0, 0
    runtime.run({background = true, tools = {}, process_owner = 'overflow-child'}, {
        on_text = function() output = output + 1 end,
        on_done = function(value) result = value; completions = completions + 1 end,
    })
    local overflow_request = #requests
    runtime.run({background = true, tools = {}, process_owner = 'overflow-parent'}, {})
    local blocking_request = #requests
    inside_tool = function()
        if tool_context.process_owner ~= 'overflow-parent' then executed = executed + 1; return end
        tool_reply(overflow_request)
        if limit == 'bytes' then
            requests[overflow_request].callback(string.rep('x', 10 * 1024 * 1024 + 1), false)
        else
            for _ = 1, 4097 do requests[overflow_request].callback(': keepalive\n\n', false) end
        end
        assert(result == nil and output == 0 and executed == 0)
    end
    tool_reply(blocking_request)
    _G.agent_background_poll()
    assert(result and result.ok == false and result.error:find('Deferred stream buffer limit exceeded', 1, true))
    assert(cancelled[overflow_request] and completions == 1 and output == 0 and executed == 0)
    delta(overflow_request, {content = 'late success'})
    requests[overflow_request].callback(nil, true)
    _G.agent_background_poll()
    assert(completions == 1 and output == 0 and executed == 0)
    runtime.shutdown_background()
end
inside_tool = nil
-- Exceptions restore both Lua and native paths before propagating.
runtime.run({background = true, tools = {}, workdir = '/error', workspace_root = '/error'}, {
    on_text = function() error('fixture callback error') end,
})
local success, failure = pcall(delta, #requests, {content = 'fail'})
assert(not success and tostring(failure):find('fixture callback error', 1, true))
assert(capstan.workdir == '/other' and native_dir == '/other')
runtime.shutdown_background()
print('background runtime tests passed')
