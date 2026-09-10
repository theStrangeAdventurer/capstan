-- Standalone Lua has no C task adapter; preserve it when run by the host.
agent = agent or {}
local tasks = require('agent.tasks')
local json = require('vendor.rxi.json')
local store = {json=''}
tasks.use_store(store)
assert(tasks.summary() == '')
assert(#tasks.read().tasks == 0)
local messages = {{role='system',content='base'}, {role='user',content='request'}}
assert(tasks.refresh(messages) == nil and #messages == 2)
local function item(id, status, result)
    return {id=id, title='Задача '..id, status=status or 'pending', criteria='Tests pass', result=result or ''}
end
local plan = assert(tasks.update({revision=0,tasks={item('one'),item('two')}}))
assert(plan.revision == 1 and #plan.tasks == 2)
local saved = store.json
assert(not tasks.update({revision=0,tasks={item('one')}}))
assert(store.json == saved)
assert(not tasks.update({revision=1,tasks={item('one'),item('one')}}))
assert(not tasks.update({revision=1,tasks={item('one','completed')}}))
assert(not tasks.update({revision=1,tasks={item('one','blocked')}}))
assert(not tasks.update({revision=1,tasks={item('one','cancelled')}}))
assert(not tasks.update({revision=1,tasks={item('one','invalid')}}))
assert(not tasks.update({revision=1,tasks={item('bad id')}}))
assert(not tasks.update({revision=1,tasks={}}))
assert(not tasks.update({revision=1,tasks={invalid=item('three')}}))
assert(store.json == saved)
plan = assert(tasks.update({revision=1,tasks={item('one','in_progress')}}))
assert(plan.tasks[2].id == 'two')
plan = assert(tasks.update({revision=2,tasks={item('one','completed','make test passed')}}))
assert(tasks.summary():find('1/2',1,true))
local injected = tasks.refresh(messages)
assert(#messages == 3 and injected.content:find('make test passed',1,true))
assert(tasks.refresh(messages,injected,1) == nil and #messages == 2)
-- Compact is represented by completely replacing history; state remains.
messages = {{role='user', content='compacted'}}
injected = tasks.refresh(messages)
assert(#messages == 2 and injected.content:find('"revision":3',1,true))
-- A second ACP session has independent tasks; selecting the first restores them.
local second = {json=''}
tasks.use_store(second)
assert(#tasks.read().tasks == 0)
assert(tasks.refresh({}) == nil)
tasks.use_store(store)
assert(tasks.read().revision == 3)
-- Decoded input/results are copies, never aliases into live state.
plan.tasks[1].title = 'modified copy'
assert(tasks.read().tasks[1].title ~= 'modified copy')
-- The view is a read-only adapter using unified config, not plan metadata.
local original_capstan = capstan
local before_view = store.json
capstan = nil
assert(tasks.view().expanded == true)
capstan = {config={tasks={expanded_by_default=false}}}
local view = tasks.view()
assert(view.expanded == false and view.summary == 'Tasks 1/2')
assert(view.items[1].mark == '✓' and view.items[2].mark == '○')
capstan.config.tasks.expanded_by_default = true
assert(tasks.view().expanded == true)
assert(store.json == before_view)
local view_store = {json=''}
tasks.use_store(view_store)
assert(#tasks.view().items == 0 and tasks.view().summary == '')
-- Closure affects only the default view, never persisted data or context.
for _, default in ipairs({'unset', true, false}) do
    capstan = default ~= 'unset' and {config={tasks={expanded_by_default=default}}} or nil
    for _, statuses in ipairs({
        {'completed', 'completed'}, {'completed', 'cancelled'},
        {'cancelled', 'cancelled'}, {'completed', 'blocked'},
        {'cancelled', 'pending'}, {'completed', 'in_progress'},
    }) do
        view_store.json = ''
        assert(tasks.view().expanded == (default ~= false))
        assert(tasks.update({revision=0, tasks={
            item('a', statuses[1], 'reason'), item('b', statuses[2], 'reason'),
        }}))
        local before = view_store.json
        local context = tasks.refresh({}).content
        local closed = statuses[2] == 'completed' or statuses[2] == 'cancelled'
        local done = statuses[1] == 'completed' and 1 or 0
        if statuses[2] == 'completed' then done = done + 1 end
        local prefix = string.format('Tasks %d/2', done)
        view = tasks.view()
        assert(view.expanded == (not closed and default ~= false))
        assert(view.summary == prefix .. (closed and
            (done == 2 and ' · completed' or ' · closed with cancellations') or ''))
        assert(#view.items == 2 and tasks.summary() == prefix)
        assert(view_store.json == before and tasks.refresh({}).content == context)
        -- Reopening a closed plan restores the configured default.
        assert(tasks.update({revision=1, tasks={item('b', 'blocked', 'Needs input')}}))
        assert(tasks.view().expanded == (default ~= false))
    end
end
capstan = {config={tasks={expanded_by_default=true}}}
view_store.json = ''
local updates = {}
for i, status in ipairs({'pending','in_progress','completed','blocked','cancelled'}) do
    updates[i] = item('view'..i, status, 'reason')
    updates[i].title = 'Title\n\t\27 wrapped'
end
assert(tasks.update({revision=0, tasks=updates}))
view = tasks.view()
for i, mark in ipairs({'○','◐','✓','!','−'}) do
    assert(view.items[i].mark == mark)
    assert(not view.items[i].title:find('%c'))
end
-- Exercise the production UTF-8 locale, including every Cyrillic letter.
local previous_locale = os.setlocale(nil, 'ctype')
assert(os.setlocale('en_US.UTF-8', 'ctype') or os.setlocale('C.UTF-8', 'ctype'))
local titles = {
    'АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ абвгдеёжзийклмнопрстуфхцчшщъыьэюя',
    'Зафиксировать контракт OpenTelemetry — café 中文 🚀',
    'До\1\9\10\13\27\31\127после',
}
local expected = {titles[1], titles[2], 'До' .. string.rep(' ', 7) .. 'после'}
local unicode_store = {json=''}
tasks.use_store(unicode_store)
local unicode_items = {}
for i, title in ipairs(titles) do
    unicode_items[i] = {id='unicode'..i, title=title, status='pending'}
end
assert(tasks.update({revision=0, tasks=unicode_items}))
local persisted = unicode_store.json
for i, entry in ipairs(tasks.view().items) do
    assert(entry.title == expected[i], entry.title)
    assert(tasks.read().tasks[i].title == titles[i])
end
assert(unicode_store.json == persisted)
assert(os.setlocale(previous_locale, 'ctype'))
tasks.use_store(view_store)
view_store.json = '{broken'
assert(tasks.view().summary == 'Tasks: error' and #tasks.view().items == 0)
capstan = original_capstan
tasks.use_store(store)
-- Corrupt state is surfaced rather than silently replaced.
store.json = '{broken'
assert(not tasks.read())
assert(not tasks.update({revision=0,tasks={item('new')}}))
assert(store.json == '{broken')
store.json = saved
local original = agent.tasks_set
local original_get = agent.tasks_get
local raw = saved
agent.tasks_get = function() return raw end
agent.tasks_set = function() return false end
tasks.use_store(nil)
assert(not tasks.update({revision=1,tasks={item('one','in_progress')}}))
assert(raw == saved)
agent.tasks_get, agent.tasks_set = original_get, original
-- Bound input, invalid NUL, plan count and serialized size.
tasks.use_store(store)
local large = {}
for i=1,101 do large[i] = item('task'..i) end
assert(not tasks.update({revision=1,tasks=large}))
local bad = item('one'); bad.title = 'x\0y'
assert(not tasks.update({revision=1,tasks={bad}}))
local plugin = dofile('plugins/tasks.lua')
local ctx = {args={}}
function ctx:replace(ui,llm) return ui,llm end
local ui, llm = plugin.handler(ctx)
assert(ui:find('Task plan',1,true) and llm == '')
ctx.tool_args = {operation='unknown'}
local _, _, ok = plugin.handler(ctx)
assert(ok == false)
ctx.tool_args = {operation='read'}
local _, encoded = plugin.handler(ctx)
assert(json.decode(encoded).revision == 1)
-- Clear uses the same revision/storage policy and never drops unfinished work.
local clear_store = {json=''}
tasks.use_store(clear_store)
for _, status in ipairs({'pending','in_progress','blocked'}) do
    clear_store.json = ''
    assert(tasks.update({revision=0,tasks={item('open',status,'reason')}}))
    local unchanged = clear_store.json
    assert(not tasks.clear({revision=1}))
    assert(clear_store.json == unchanged)
end
assert(tasks.update({revision=1,tasks={item('open','cancelled','User abandoned work'),item('done','completed','Tests passed')}}))
local closed = clear_store.json
assert(not tasks.clear({revision=1}))
assert(not tasks.clear({}))
assert(clear_store.json == closed)
messages = {{role='user',content='next task'}}
injected = tasks.refresh(messages)
ctx.tool_args = {operation='clear',revision=2}
local _, cleared = plugin.handler(ctx)
assert(json.decode(cleared).revision == 3 and #json.decode(cleared).tasks == 0)
assert(clear_store.json:find('"tasks":[]',1,true))
assert(tasks.summary() == '' and #tasks.view().items == 0)
assert(tasks.refresh(messages,injected) == nil and #messages == 1)
assert(not tasks.update({revision=2,tasks={item('stale')}}))
assert(tasks.update({revision=3,tasks={item('new','completed','Done')}}))
ctx.tool_args, ctx.args = nil, {'clear','extra'}
assert(plugin.handler(ctx):find('Usage:',1,true))
assert(tasks.read().revision == 4)
ctx.args = {'clear'}
local cleared_ui, cleared_llm = plugin.handler(ctx)
assert(cleared_ui == 'Task plan cleared.' and cleared_llm == '')
assert(tasks.read().revision == 5 and #tasks.read().tasks == 0)
assert(tasks.clear({revision=5}).revision == 6)
tasks.use_store(store)
assert(store.json == saved) -- another session was untouched
clear_store.json = '{broken'
tasks.use_store(clear_store)
assert(not tasks.clear({revision=0}) and clear_store.json == '{broken')
-- A valid closed plan still survives a failing persistence adapter.
tasks.use_store(nil)
agent.tasks_get = function() return closed end
agent.tasks_set = function() return false end
local failed, failure = tasks.clear({revision=2})
assert(not failed and failure:find('Could not save',1,true))
assert(agent.tasks_get() == closed)
agent.tasks_get, agent.tasks_set = original_get, original
tasks.use_store(nil)
