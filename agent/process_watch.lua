-- Session-owned timers. No sleeps, model calls or HTTP pumping in this module.
local M = {}
local watches = {}
local function now() return capstan.now_ms() end
function M.watch(id, owner, interval)
    interval = tonumber(interval) or 300
    if interval ~= interval or interval < 1 or interval > 86400 then
        error('Watch interval must be between 1 and 86400 seconds')
    end
    local snapshot = tools.processes('get', id) -- repeats native access check
    if not snapshot then error('Managed process snapshot unavailable') end
    local key = owner .. '\0' .. id
    local count = 0
    for _ in pairs(watches) do count = count + 1 end
    if not watches[key] and count >= 32 then error('Process watch capacity reached (32)') end
    watches[key] = {id = id, owner = owner, interval = interval * 1000,
        next_at = now() + interval * 1000}
    return {watching = true, id = id, interval = interval}
end
function M.cancel_owner(owner)
    for key, watch in pairs(watches) do
        if not owner or watch.owner == owner then watches[key] = nil end
    end
end
function M.collect(owner)
    local events = tools.process_events(owner)
    local seen = {}
    for _, event in ipairs(events) do seen[event.id] = true end
    if next(watches) == nil then return events end
    local previous_owner, previous_task = tools.process_scope(owner)
    local ok, err = pcall(function()
        for key, watch in pairs(watches) do
            if watch.owner == owner then
                if seen[watch.id] then
                    watches[key] = nil
                elseif now() >= watch.next_at then
                    local read, snapshot = pcall(tools.processes, 'get', watch.id)
                    if not read or type(snapshot) ~= 'table' then
                        events[#events + 1] = {id = watch.id, watch_error = 'Managed process unavailable'}
                        watches[key] = nil
                    else
                        snapshot.watch_tick = true
                        events[#events + 1] = snapshot
                        if snapshot.running == false then watches[key] = nil
                        else watch.next_at = now() + watch.interval end
                    end
                end
            end
        end
        -- Native get polls the process manager too: an exit discovered above
        -- may have queued a completion after the first event drain. Consume it
        -- now and replace the timer snapshot rather than delivering it twice.
        for _, event in ipairs(tools.process_events(owner)) do
            local replaced = false
            for i, existing in ipairs(events) do
                if existing.id == event.id then
                    events[i] = event
                    replaced = true
                    break
                end
            end
            if not replaced then events[#events + 1] = event end
            watches[owner .. '\0' .. event.id] = nil
        end
    end)
    tools.process_scope(previous_owner, previous_task)
    if not ok then error(err, 0) end
    return events
end
-- UI never includes command lines, paths, PIDs, JSON or agent instructions.
function M.describe(events)
    local running, completed, attention = 0, 0, 0
    for _, event in ipairs(events) do
        local exit = event.exit or event.exit_code
        if event.watch_error or event.timed_out or
           event.status == 'failed' or (type(exit) == 'number' and exit ~= 0) then
            attention = attention + 1
        elseif event.running then running = running + 1
        else completed = completed + 1 end
    end
    local parts = {}
    if running > 0 then parts[#parts + 1] = running .. ' running' end
    if completed > 0 then parts[#parts + 1] = completed .. ' finished' end
    if attention > 0 then parts[#parts + 1] = attention .. ' need attention' end
    return 'Background: ' .. table.concat(parts, ', ') .. '.'
end
return M
