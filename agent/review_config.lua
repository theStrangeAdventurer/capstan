-- Canonical per-run completion-review configuration. Runtime owns execution.
local M = {}

local function finite(value)
    return type(value) == 'number' and value == value and value > -math.huge and value < math.huge
end

local function integer(value)
    return finite(value) and value % 1 == 0
end

local function invalid(field, requirement)
    return nil, 'agent.completion_review' .. field .. ' must be ' .. requirement
end

function M.normalize(value, depth)
    if depth == nil then depth = 0 end
    if not integer(depth) or depth < 0 then
        return invalid(' depth', 'a nonnegative integer')
    end
    if value ~= nil and type(value) ~= 'boolean' and type(value) ~= 'table' then
        return invalid('', 'a boolean or table')
    end
    local source = type(value) == 'table' and value or {}
    local allowed = {enabled=true, max_fix_cycles=true, max_duration_sec=true, max_requests=true, reviewer=true}
    for key in pairs(source) do
        if not allowed[key] then
            return nil, 'Unknown agent.completion_review field: ' .. tostring(key)
        end
    end
    if source.enabled ~= nil and type(source.enabled) ~= 'boolean' then
        return invalid('.enabled', 'a boolean')
    end
    local result = {
        enabled = value ~= nil and value ~= false,
        max_fix_cycles = 2,
        max_duration_sec = 900,
        -- Resolved against the effective orchestrator limit at run creation.
        reviewer = {},
    }
    if source.enabled ~= nil then result.enabled = source.enabled end
    for _, field in ipairs({'max_fix_cycles', 'max_duration_sec', 'max_requests'}) do
        if source[field] ~= nil then result[field] = source[field] end
    end
    if not integer(result.max_fix_cycles) or result.max_fix_cycles < 0 or result.max_fix_cycles > 30 then
        return invalid('.max_fix_cycles', 'an integer between 0 and 30')
    end
    if not finite(result.max_duration_sec) or result.max_duration_sec <= 0 then
        return invalid('.max_duration_sec', 'a positive finite number')
    end
    if result.max_requests ~= nil and (not integer(result.max_requests) or result.max_requests <= 0) then
        return invalid('.max_requests', 'a positive integer')
    end
    if source.reviewer ~= nil then
        if type(source.reviewer) ~= 'table' then return invalid('.reviewer', 'a table') end
        for key in pairs(source.reviewer) do
            if key ~= 'max_turns' then
                return nil, 'Unknown agent.completion_review.reviewer field: ' .. tostring(key)
            end
        end
        if source.reviewer.max_turns ~= nil then result.reviewer.max_turns = source.reviewer.max_turns end
    end
    if result.reviewer.max_turns ~= nil and
        (not integer(result.reviewer.max_turns) or result.reviewer.max_turns <= 0) then
        return invalid('.reviewer.max_turns', 'a positive integer')
    end
    -- Validate even disabled/nested configurations: bad values must not disappear.
    if depth > 0 then result.enabled = false end
    return result
end

-- Called only with validated settings and the runtime's effective parent limit.
function M.resolve(settings, parent_max_turns)
    settings.reviewer.max_turns = settings.reviewer.max_turns or parent_max_turns
    settings.max_requests = settings.max_requests or
        (settings.reviewer.max_turns * (settings.max_fix_cycles + 1) + parent_max_turns)
    return settings
end

return M
