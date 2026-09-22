-- Internal async scheduler contract; no network or native dependencies.
local json = require('vendor.rxi.json')
local registry, serial, pending, requests, hooks_count = {}, 0, {}, 0, 0
local fail_update, fail_register, fail_release
local function count()
    local n = 0; for _ in pairs(registry) do n = n + 1 end; return n
end
package.loaded['agent.ui'] = {append = function() error('internal work must be silent') end}
package.loaded['agent.telemetry'] = {start = function() return {} end, finish = function() end}
package.loaded['agent.hooks'] = {run = function(_, ctx) hooks_count = hooks_count + 1; return ctx end}
capstan = {config = {subagents = {max_concurrent_cap = 2}},
    workdir = '/workspace', workspace_root = '/workspace', now_ms = function() return 0 end,
    agent = {run = function(opts, cb)
        requests = requests + 1
        local entry = {opts = opts, cb = cb}; pending[#pending + 1] = entry
        return true, nil, function()
            entry.cancelled = true
            cb.on_done({ok = false, error = 'cancelled'})
        end
    end}}
tools = {
    background_register = function(fields)
        if fail_register and count() >= fail_register then error('registry full') end
        assert(count() < 128)
        serial = serial + 1
        local id = 'bg-' .. serial
        fields.status = 'queued'; registry[id] = fields
        return id
    end,
    background_update = function(id, fields)
        local previous = assert(registry[id])
        assert(previous.status == 'queued' or previous.status == 'running', 'terminal mutation')
        if fail_update and fail_update(id, fields) then error('publication failure') end
        for k, v in pairs(fields) do previous[k] = v end
        return true
    end,
    background_cancelled = function(id) return not registry[id] or registry[id].cancelled == true end,
    background_release = function(id)
        if fail_release and fail_release(id) then error('release failure') end
        local record = assert(registry[id])
        assert(record.status ~= 'queued' and record.status ~= 'running')
        registry[id] = nil
        return true
    end,
}
local M = require('agent.subagents')
local ctx = {process_owner = 'owner', provider_name = 'p', provider = {model = 'm'},
    tools = {{['function'] = {name = 'file_read'}}}, permission_scope = {allowed_tools = {file_read = true}}}
local args = {tasks = {{id = 'review', task = 'read', tools = {'file_read'}}}}
local function submit(options)
    local h, err = M.submit(args, ctx, options); assert(h, err); return h
end
local h = submit()
assert(type(h) == 'table' and next(h) == nil)
assert(M.result(h).status == 'queued' and not M.result(h).done and requests == 0)
assert(count() == 2)
for _, r in pairs(registry) do assert(r.notify == false) end
assert(not M.release(h) and count() == 2)
assert(not M.result({}) and not M.cancel({}) and not M.release({}))
ctx.provider.model = 'changed'; ctx.permission_scope.allowed_tools.file_read = false
M.poll()
assert(requests == 1 and pending[1].opts.provider_snapshot.model == 'm')
assert(pending[1].opts.permission_scope.allowed_tools.file_read)
pending[1].cb.on_done({ok = true, text = 'clean', turns = 1})
assert(not M.result(h).done, 'delivery waits for scheduler poll')
M.poll()
local result = M.result(h)
assert(result.done and result.status == 'completed' and result.result.results[1].text == 'clean')
result.result.results[1].text = 'mutated'
assert(M.result(h).result.results[1].text == 'clean')
assert(M.cancel(h) and M.release(h) and count() == 0)
assert(not M.result(h) and not M.release(h))
pending[1].cb.on_done({ok = false, error = 'HTTP 503 late'})
M.poll(); assert(requests == 1 and count() == 0)

-- Bound lifetime never cancels an independent group with the same owner.
local detached, ok = M.run({background = true, tasks = args.tasks}, ctx)
assert(ok); detached = json.decode(detached)
local cancelled = false
local bound = submit({is_cancelled = function() return cancelled end})
M.poll(); assert(requests == 3)
cancelled = true; M.poll()
assert(M.result(bound).status == 'cancelled' and pending[3].cancelled)
assert(not pending[2].cancelled and registry[detached.id].status == 'running')
assert(M.release(bound))
pending[2].cb.on_done({ok = true, text = 'independent'})
M.poll(); assert(registry[detached.id].status == 'completed' and registry[detached.id].notify)
local retained = count()

-- Queued cancellation and a throwing lifetime callback fail closed.
local queued = submit({is_cancelled = function() error('owner ended') end})
M.poll(); assert(M.result(queued).status == 'cancelled' and requests == 3)
assert(M.release(queued) and count() == retained)

-- Explicit cancellation invalidates attempts/retries and is idempotent.
local retry = submit(); M.poll()
local attempt = pending[#pending]
attempt.cb.on_done({ok = false, error = 'HTTP 503 transient'})
assert(M.cancel(retry) and M.cancel(retry)); M.poll()
assert(M.result(retry).status == 'cancelled' and requests == 4)
assert(M.release(retry))
attempt.cb.on_done({ok = true, text = 'late'})
M.poll(); assert(requests == 4)

-- Internal callers cannot silently bypass disabled capabilities.
capstan.config.capabilities = {subagents = false}
local denied, err = M.submit(args, ctx)
assert(not denied and err:find('capability disabled', 1, true))
capstan.config.capabilities = nil
assert(not M.submit(args, ctx, {is_cancelled = true}))

-- Terminal publication retries do not rerun hooks or report false success.
local bad = submit(); M.poll()
local hook_before = hooks_count
fail_update = function(id, fields)
    return registry[id].kind == 'subagent_group' and fields.status == 'completed'
end
pending[#pending].cb.on_done({ok = true, text = 'original'})
M.poll()
assert(not M.result(bad).done and M.result(bad).publication_error)
assert(hooks_count == hook_before + 1)
fail_update = nil; M.poll()
assert(M.result(bad).done and M.result(bad).status == 'failed')
assert(not M.result(bad).result.ok and hooks_count == hook_before + 1)
assert(M.release(bad))

-- A task publication failure also remains pending until native state settles.
local task_bad = submit(); M.poll()
fail_update = function(id, fields)
    return registry[id].kind == 'subagent' and fields.status == 'completed'
end
pending[#pending].cb.on_done({ok = true, text = 'not yet published'})
M.poll(); assert(not M.result(task_bad).done)
fail_update = nil; M.poll()
assert(M.result(task_bad).done and not M.result(task_bad).result.ok)
assert(M.release(task_bad))

-- Partial registration on capacity exhaustion leaves no unreachable records.
fail_register = retained + 2
local no_capacity, capacity_error = M.submit({tasks = {{id = 'a'}, {id = 'b'}}}, ctx)
assert(not no_capacity and capacity_error:find('registry full', 1, true) and count() == retained)
fail_register = nil

-- Partial registration is cancelled and reclaimed, even across a publication failure.
fail_register = retained + 2
fail_update = function() return true end
local missing = M.submit({tasks = {{id = 'a'}, {id = 'b'}}}, ctx)
assert(not missing and count() == retained + 2)
fail_register, fail_update = nil, nil
M.poll(); assert(count() == retained)

-- Partial release can be retried without touching an already-freed child ID.
local release_bad = submit(); M.poll()
pending[#pending].cb.on_done({ok = true, text = 'done'}); M.poll()
fail_release = function(id) return registry[id].kind == 'subagent_group' end
assert(not M.release(release_bad) and count() == retained + 1)
fail_release = nil
assert(M.release(release_bad) and count() == retained)

-- Long series reuses both native records and Lua group slots.
for _ = 1, 300 do
    local cycle = submit(); M.poll()
    pending[#pending].cb.on_done({ok = true, text = 'clean'}); M.poll()
    assert(M.result(cycle).done and M.release(cycle) and count() == retained)
end
-- Shutdown releases internal contexts, but preserves public group history.
local closing = submit(); M.poll()
M.shutdown()
assert(pending[#pending].cancelled and not M.result(closing) and count() == retained)
print('internal subagents: passed')

-- Paging belongs only to the internal reviewer schema, not the parent tool.
local reviewer={snapshot={},budget={}}
capstan.config.subagents.max_turns_cap=2
args.tasks[1].max_turns=87
local schema_handle=submit({reviewer=reviewer})
M.poll()
local child=pending[#pending]
assert(child.opts.reviewer==reviewer and child.opts.max_turns==87)
local spec=child.opts.tools[1]['function']
assert(spec.parameters.properties.offset.minimum==0)
assert(spec.parameters.properties.limit.maximum==48000)
assert(spec.description:find('Batch paths must omit offset and limit',1,true))
assert(ctx.tools[1]['function'].parameters==nil)
child.cb.on_done({ok=true,text='clean',turns=1})
M.poll(); assert(M.release(schema_handle))
print('internal_subagents: reviewer-only paging schema passed')
