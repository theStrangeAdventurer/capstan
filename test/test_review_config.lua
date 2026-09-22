local config = require('agent.review_config')
local function normalized(value, depth)
    local result, err = config.normalize(value, depth)
    assert(result, err)
    assert(err == nil)
    return result
end
local function invalid(value, field, depth)
    local result, err = config.normalize(value, depth)
    assert(result == nil and type(err) == 'string' and #err > 0)
    if field then assert(err:find(field, 1, true), err) end
end

assert(not normalized(nil).enabled)
assert(not normalized(false).enabled)
local defaults = normalized(true)
assert(defaults.enabled and defaults.max_fix_cycles == 2)
assert(defaults.max_duration_sec == 900 and defaults.max_requests == nil and defaults.reviewer.max_turns == nil)
assert(normalized({}).enabled)
assert(not normalized({enabled=false}).enabled)
assert(normalized({enabled=true}).enabled)
local source = {max_fix_cycles=0, max_duration_sec=0.5, max_requests=1, reviewer={max_turns=1}}
local result = normalized(source)
assert(result.max_fix_cycles == 0 and result.max_duration_sec == 0.5)
assert(result.max_requests == 1 and result.reviewer.max_turns == 1)
result.reviewer.max_turns = 100
assert(source.reviewer.max_turns == 1 and normalized(true).reviewer.max_turns == nil)
for _, turns in ipairs({8, 80, 250}) do
    local effective = config.resolve(normalized(true), turns)
    assert(effective.reviewer.max_turns == turns and effective.max_requests == turns * 4)
end
local explicit = config.resolve(normalized({max_requests=3, reviewer={max_turns=7}}), 90)
assert(explicit.max_requests == 3 and explicit.reviewer.max_turns == 7)
assert(normalized({max_fix_cycles=30}).max_fix_cycles == 30)
assert(not normalized(true, 1).enabled)
assert(not normalized({enabled=true}, 2).enabled)
assert(normalized(true, 0).enabled)

for _, value in ipairs({0, 1, '', 'true', function() end}) do invalid(value) end
for _, value in ipairs({0, 'false', {}}) do invalid({enabled=value}, '.enabled') end
for _, value in ipairs({-1, 31, 0.5, '2', false, {}, math.huge, -math.huge, 0/0}) do
    invalid({max_fix_cycles=value}, '.max_fix_cycles')
end
for _, value in ipairs({0, -1, '900', false, {}, math.huge, -math.huge, 0/0}) do
    invalid({max_duration_sec=value}, '.max_duration_sec')
end
for _, value in ipairs({0, -1, 1.5, '6', false, {}, math.huge, -math.huge, 0/0}) do
    invalid({max_requests=value}, '.max_requests')
    invalid({reviewer={max_turns=value}}, '.reviewer.max_turns')
end
for _, value in ipairs({false, true, 6, 'reviewer'}) do invalid({reviewer=value}, '.reviewer') end
invalid({enabled=false, max_requests=0}, '.max_requests')
invalid({max_requests=0}, '.max_requests', 1)
invalid({typo=true}, 'typo')
invalid({reviewer={typo=6}}, 'typo')
for _, depth in ipairs({-1, 0.5, '1', false, math.huge, 0/0}) do invalid(true, 'depth', depth) end

local plugin = dofile('plugins/completion_review.lua')
assert(plugin.id == 'completion_review' and plugin.command == nil)
assert(plugin.tool.name == 'request_completion' and plugin.tool.permission == false)
local schema = plugin.tool.parameters
assert(schema.type == 'object' and schema.additionalProperties == false)
assert(schema.required[1] == 'status' and schema.required[2] == 'text')
assert(schema.properties.text.type == 'string' and schema.properties.status.type == 'string')
assert(table.concat(schema.properties.status.enum, ',') == 'ready,question,blocked')
local old_entry, old_agent = _G.agent_entry, _G.agent
_G.agent_entry = function() error('must not dispatch a nested run') end
_G.agent = setmetatable({}, {__index=function() error('must not call global completion') end})
for _, status in ipairs({'ready', 'question', 'blocked'}) do
    local ui, llm, ok = plugin.handler({tool_args={status=status,text='Result'}})
    assert(ok == false and ui == llm and ui:find('runtime', 1, true))
end
local _, _, ok = plugin.handler(nil)
assert(ok == false)
_G.agent_entry, _G.agent = old_entry, old_agent
print('review config: defaults, validation, depth isolation and completion tool contract passed')
