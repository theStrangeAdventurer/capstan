package.path = './?.lua;' .. package.path
local clock, owner, events = 0, 'ambient', {}
capstan = {now_ms = function() return clock end}
local snapshots = {p = {id = 'p', status = 'running', running = true}}
tools = {
    processes = function(_, id)
        assert(owner == 'a', 'timer crossed owner scope')
        if not snapshots[id] then error('missing') end
        local copy = {}; for k,v in pairs(snapshots[id]) do copy[k] = v end
        return copy
    end,
    process_events = function(o)
        local result = events[o] or {}; events[o] = nil; return result
    end,
    process_scope = function(o) local old = owner; owner = o; return old end,
}
local watch = require('agent.process_watch')
owner = 'a'
assert(watch.watch('p', 'a', 300).watching)
owner = 'ambient'
clock = 299999
assert(#watch.collect('a') == 0 and owner == 'ambient')
clock = 300000
assert(#watch.collect('b') == 0)
local tick = watch.collect('a')
assert(#tick == 1 and tick[1].watch_tick and owner == 'ambient')
assert(#watch.collect('a') == 0)
-- Completion beats timer and removes the watcher; exactly once.
events.a = {{id = 'p', status = 'exited', running = false, exit_code = 1}}
assert(#watch.collect('a') == 1)
clock = 900000
assert(#watch.collect('a') == 0)
owner = 'a'
assert(not pcall(watch.watch, 'p', 'a', 0))
assert(not pcall(watch.watch, 'p', 'a', 0/0))
watch.watch('p', 'a', 1)
owner = 'ambient'; snapshots.p = nil; clock = clock + 1000
assert(watch.collect('a')[1].watch_error and owner == 'ambient')
assert(#watch.collect('a') == 0)
snapshots.p = {id = 'p', running = true}
owner = 'a'; watch.watch('p', 'a', 1); owner = 'ambient'
watch.cancel_owner('a'); clock = clock + 1000
assert(#watch.collect('a') == 0)
-- Exit discovered by get after the initial completion drain must be consumed
-- in this collection, not redelivered on the next foreground continuation.
local original_get = tools.processes
owner = 'a'; watch.watch('p', 'a', 1); owner = 'ambient'
tools.processes = function(action, id)
    local snapshot = original_get(action, id)
    snapshot.running, snapshot.status = false, 'exited'
    events.a = {{id = id, running = false, status = 'exited', exit_code = 1}}
    return snapshot
end
clock = clock + 1000
local completion = watch.collect('a')
assert(#completion == 1 and completion[1].exit_code == 1 and not completion[1].watch_tick)
assert(owner == 'ambient' and #watch.collect('a') == 0)
clock = clock + 1000
assert(#watch.collect('a') == 0)
tools.processes = original_get
print('process watch tests passed')
