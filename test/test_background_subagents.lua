-- Standalone scheduler contract; no native libraries or network required.
local json = require('vendor.rxi.json')
local pending, calls, registry, tick, ui_calls, hook = {}, {}, {}, 0, 0, nil
package.loaded['agent.ui'] = {append = function() ui_calls = ui_calls + 1 end}
package.loaded['agent.telemetry'] = {start = function() return {} end, finish = function() end}
package.loaded['agent.hooks'] = {run = function(_, ctx) if hook then hook(ctx) end; return ctx end}
capstan = {config = {subagents = {max_concurrent_cap = 2, max_result_bytes = 256}},
    workdir = '/captured', workspace_root = '/captured', now_ms = function() return tick end,
    agent = {run = function(opts, cb)
        calls[#calls + 1] = opts
        local entry = {opts = opts, cb = cb}; pending[#pending + 1] = entry
        return true, nil, function() entry.cancelled = true; cb.on_done({ok = false, error = 'cancelled'}) end
    end}}
tools = {
    background_register = function(fields)
        local id = 'bg-' .. (#registry + 1); registry[#registry + 1] = fields; registry[id] = fields
        return id
    end,
    background_update = function(id, fields)
        local previous = assert(registry[id])
        assert(previous.status ~= 'completed' and previous.status ~= 'failed' and previous.status ~= 'cancelled', 'terminal snapshots are immutable')
        assert(#fields.output <= 65536)
        for k,v in pairs(fields) do previous[k] = v end
    end,
    background_cancelled = function(id) return registry[id].cancel == true end,
    processes = function(_, id) assert(registry[id], 'unknown ID'); local out = {}; for k,v in pairs(registry[id]) do out[k] = v end; return out end,
}
http = {poll = function() tick = tick + 1000 end, wait_frame = function() end}
local M = require('agent.subagents')
local ctx = {process_owner = 'owner', provider_name = 'p', provider = {model = 'm'},
    profile = {id = 'plan'}, tools = {{['function'] = {name = 'file_read'}}},
    permission_scope = {allowed_tools = {file_read = true}}, system_prompt = 'captured prompt',
    telemetry_tool = {ended = true}, is_cancelled = function() return true end}
local function launch(tasks, concurrency)
    local text, ok = M.run({background = true, tasks = tasks or {{id = 'a', task = 'read'}}, max_concurrent = concurrency}, ctx)
    assert(ok, text); return json.decode(text)
end
local first = launch({{id='a',task='one'}, {id='b',task='two'}, {id='c',task='three'}}, 1)
local second = launch()
assert(#calls == 0 and tick == 0 and ui_calls == 0, 'launch must not wait or dispatch')
ctx.permission_scope.allowed_tools.file_read = false; ctx.profile.id = 'changed'; capstan.workdir = '/changed'
M.poll()
assert(#calls == 2 and calls[1].workdir == '/captured' and calls[1].profile.id == 'plan')
assert(calls[1].permission_scope.allowed_tools.file_read and calls[1].system_prompt == 'captured prompt')
assert(calls[1].process_owner == 'owner' and calls[1].background and not calls[1].is_cancelled())
M.poll(); assert(#calls == 2, 'global and per-group limits')
pending[1].cb.on_done({ok=false,error='HTTP 503 transient'})
pending[1].cb.on_done({ok=true,text='late'})
assert(#calls == 2, 'callbacks must not recursively retry')
M.poll(); assert(#calls == 3 and calls[3].subagent_attempt == 2)
pending[3].cb.on_text('token='); pending[3].cb.on_text('dummy-secret\n' .. string.rep('x', 900))
pending[3].cb.on_done({ok=true,turns=2})
local result = json.decode(registry[first.tasks[1].background_id].output)
assert(#result.text <= 256 and result.text_truncated and not result.text:find('dummy-secret',1,true))
M.poll(); assert(#calls == 4)
registry[first.group_id].cancel = true; M.poll()
assert(pending[4].cancelled and registry[first.group_id].status == 'cancelled')
assert(registry[first.tasks[3].background_id].status == 'cancelled')
pending[4].cb.on_done({ok=false,error='HTTP 503 late'}); M.poll(); assert(#calls == 4)
M.cancel_owner('other'); assert(not pending[2].cancelled)
M.cancel_owner('owner'); assert(pending[2].cancelled and registry[second.group_id].status == 'cancelled')
assert(ui_calls == 0, 'background must never append to active UI')
-- Inline callbacks, bounded retries, and post-hook sanitization.
capstan.agent.run = function(opts, cb)
    calls[#calls+1] = opts
    cb.on_done({ok=false,error='HTTP 503 exhausted'})
    cb.on_done({ok=true,text='duplicate'})
    return false, 'HTTP 503 duplicate'
end
local retry = launch()
for _=1,4 do M.poll() end
assert(registry[retry.group_id].status == 'failed')
assert(json.decode(registry[retry.tasks[1].background_id].output).attempts == 3)
hook = function(c) c.result.results[1].text = 'token=dummy-hook\n' .. string.rep('y',1000) end
capstan.agent.run = function(_, cb) cb.on_done({ok=true,text='ok'}); return true end
local hooked = launch(); M.poll()
local output = json.decode(registry[hooked.group_id].output)
assert(#output.results[1].text <= 256 and not output.results[1].text:find('dummy-hook',1,true))
hook = nil
-- Foreground requires no registry and preserves shared permission scope.
local native = tools; tools = nil
ctx.is_cancelled, ctx.telemetry_tool = nil, nil
capstan.agent.run = function(opts, cb)
    assert(opts.permission_scope == ctx.permission_scope)
    cb.on_done({ok=true,text='foreground',turns=1}); return true
end
local foreground, ok = M.run({tasks={{id='fg',task='read'}}},ctx)
assert(ok and json.decode(foreground).results[1].text == 'foreground' and ui_calls > 0)
tools = native
-- Explicit wait is bounded, returns snapshot timeout rather than failure.
local plugin = dofile('plugins/processes.lua')
capstan.agent.run = function() return true end
local waiting = launch()
local _, raw, waited = plugin.handler({tool_args={action='wait',id=waiting.id,timeout=2}})
assert(waited and json.decode(raw).timed_out and tick == 2000)
local _, zero = plugin.handler({tool_args={action='wait',id=waiting.id,timeout=0}})
assert(json.decode(zero).timed_out and tick == 2000)
local _, _, unknown = plugin.handler({tool_args={action='wait',id='unknown',timeout=0}}); assert(not unknown)
M.shutdown(); assert(registry[waiting.id].status == 'cancelled')
local _, terminal = plugin.handler({tool_args={action='wait',id=waiting.id}})
assert(json.decode(terminal).timed_out == false)
-- Retained groups are bounded and exhaustion fails closed without dispatch.
local total = 5 -- first, second, retry, hooked, waiting
for _=total+1,128 do launch() end
local rejected, accepted = M.run({background=true,tasks={{id='overflow',task='no'}}},ctx)
assert(not accepted and rejected:find('limit',1,true))
M.shutdown()
print('background subagents: passed')
