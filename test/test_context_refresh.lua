-- Offline: vendor/lua-5.5.0/src/lua test/test_context_refresh.lua
package.path = './?.lua;./?/init.lua;' .. package.path
local function noop() end

-- Runs with an unknown provider never reach a request, so no HTTP is needed.
http = {cancel = noop}
agent = {set_info = noop, set_usage = noop, set_profile_info = noop}
popup = {error = noop}

local refreshes, logs = 0, {}
capstan = {
    version = 'test-version',
    config = {},
    workdir = '.',
    workspace_root = '.',
    refresh_context = function() refreshes = refreshes + 1 end,
    skills_summary = 'fixture skills summary',
    wiki_summary = 'fixture wiki summary',
}
package.loaded['agent.state'] = {provider = noop, profile_models = function() return {} end,
    model_for = noop, model_reasoning_effort = noop, profile_reasoning_effort = noop}
package.loaded['agent.logging'] = {runtime_log = function(_, value) logs[#logs + 1] = tostring(value) end,
    compact = tostring, safe_error = tostring, debug = noop,
    raw_logging_enabled = function() return false end}
package.loaded['agent.hooks'] = {has = function() return false end,
    run = function(_, ctx) return ctx end, install_config = noop, install_existing_plugins = noop}
package.loaded['agent.mcp'] = {}
package.loaded['agent.ui'] = {append = noop}
package.loaded['agent.tasks'] = {refresh = noop}
package.loaded['agent.tools'] = {collect = function() return {} end, names = function() return '' end,
    handle_tool_calls = function() error('unexpected tool call') end}

local runtime = require('agent.runtime')
local messages = {{role = 'user', content = 'hi'}}

-- Every top-level run rescans shared context metadata before capturing the
-- prompt, even when the run then fails before any request.
local done = nil
assert(not runtime.run({provider = 'fixture-missing', messages = messages},
    {on_done = function(result) done = result end}))
assert(refreshes == 1 and done and done.ok == false)
assert(#logs > 0 and logs[#logs]:find('unknown provider', 1, true))

-- Runs with an explicit prompt (subagents, review, dedicated runs) skip it.
assert(not runtime.run({provider = 'fixture-missing', messages = messages,
    system_prompt = 'inherited'}, {on_done = noop}))
assert(refreshes == 1)

-- A failing refresh is fail-open: the error is logged and the run proceeds.
capstan.refresh_context = function() error('refresh boom') end
local before = #logs
done = nil
assert(not runtime.run({provider = 'fixture-missing', messages = messages},
    {on_done = function(result) done = result end}))
assert(done and done.ok == false)
local found_refresh_error = false
for i = before + 1, #logs do
    if logs[i]:find('refresh boom', 1, true) then found_refresh_error = true end
end
assert(#logs > before and found_refresh_error)

-- The /refresh command refreshes and reports the current summaries.
refreshes = 0
capstan.refresh_context = function() refreshes = refreshes + 1 end
local plugin = dofile('plugins/refresh.lua')
assert(plugin.id == 'refresh' and plugin.command == '/refresh' and plugin.history == false)
local ctx = {replace = function(_, ui, llm) return ui, llm end}
local ui = plugin.handler(ctx)
assert(refreshes == 1)
assert(ui:find('refreshed', 1, true) and ui:find('fixture skills summary', 1, true) and
    ui:find('fixture wiki summary', 1, true))

-- A failing refresh surfaces the error instead of a success message.
capstan.refresh_context = function() error('refresh boom') end
ui = plugin.handler(ctx)
assert(ui:find('failed', 1, true) and ui:find('refresh boom', 1, true))

-- Runtimes without the native adapter get an explicit refusal.
capstan.refresh_context = nil
ui = plugin.handler(ctx)
assert(ui:find('unavailable', 1, true))

print('context refresh: passed (per-run trigger, explicit-prompt skip, fail-open, /refresh command)')
