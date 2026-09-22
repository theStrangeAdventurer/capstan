-- Controller policy only: real verdict/issue persistence, no HTTP or live files.
local json = require('vendor.rxi.json')
local issues = require('agent.issues')
local tasks = require('agent.tasks')
local env
package.loaded['agent.review_snapshot'] = {
    capture = function(root)
        assert(root == '/workspace')
        if env.enumeration_error then return nil, env.enumeration_error end
        local s = {version = env.version, paths = {'src/example.c'}, complete = true, releases = 0}
        function s:diff(baseline) assert(baseline); return json.array({'src/example.c'}) end
        function s:check()
            if env.enumeration_error then return false, env.enumeration_error end
            return not env.drift, 'external workspace drift'
        end
        function s:release() self.releases = self.releases + 1; assert(self.releases == 1) end
        env.snapshots[#env.snapshots + 1] = s
        return s
    end,
    acquire = function(owner)
        if env.busy then return nil, 'active writer' end
        assert(not env.owner or env.owner == owner); env.owner = owner; return true
    end,
    release = function(owner) if env.owner == owner then env.owner = nil end end,
}
package.loaded['agent.telemetry'] = {
    start = function() env.starts = env.starts + 1; return {} end,
    finish = function(_, ok, cancelled, attrs)
        env.finishes = env.finishes + 1
        env.telemetry = {ok = ok, cancelled = cancelled, attrs = attrs}
    end,
}
package.loaded['agent.subagents'] = {
    submit = function(args, ctx, opts)
        assert(opts.notify == false and opts.reviewer.budget == env.controller)
        assert(#ctx.tools == 1 and ctx.tools[1] == env.tool)
        assert(#args.tasks == 1 and args.tasks[1].tools[1] == 'file_read')
        local h = {args = args, ctx = ctx, opts = opts}
        env.groups[#env.groups + 1] = h
        return h
    end,
    result = function(h)
        assert(not h.released)
        -- Actual scheduler wire shape, including plural results.
        return h.result or {done = false}
    end,
    cancel = function(h) assert(not h.released); h.cancelled = true; return true end,
    release = function(h) assert(not h.released); h.released = true; return true end,
    discard = function() error('unexpected discard') end,
}
local controller = require('agent.completion_review')
local function report(kind, findings, checks)
    return json.encode({verdict = kind, summary = 'Review evidence unavailable or checked',
        findings = json.array(findings or {}), checks = json.array(checks or {})})
end
local function finding(id)
    return {id = id, severity = 'high', description = 'Save error ignored', evidence = 'Failed save returns success'}
end
local clean = report('clean')
local function setup(overrides)
    env = {version = 'v1', snapshots = {}, groups = {}, finals = {}, resumes = {},
        clock = 0, starts = 0, finishes = 0, cancels = 0, store = {json = ''},
        tool = {['function'] = {name = 'file_read'}}}
    issues.use_store(env.store)
    env.task_store = {json = ''}
    tasks.use_store(env.task_store)
    capstan = {now_ms = function() return env.clock end}
    local config = {max_fix_cycles = 2, max_duration_sec = 10, max_requests = 3, reviewer = {max_turns = 6}}
    for k, v in pairs(overrides or {}) do config[k] = v end
    local c = controller.new(config, {workspace_root = '/workspace', workdir = '/workspace',
        messages = json.array({}), read_tool = env.tool,
        foreground_lost = function() return env.foreground_lost end,
        waiting = function() env.waiting=true end,
        cancelled = function() return env.cancelled end,
        status = function(label) env.status = label end,
        finalize = function(text, kind, reason) env.finals[#env.finals + 1] = {text = text, kind = kind, reason = reason} end,
        resume = function(raw) env.resumes[#env.resumes + 1] = json.decode(raw) end,
        cancel = function() env.cancels = env.cancels + 1 end})
    env.controller = c
    return c
end
local function queue(c, draft)
    c:attempt(draft or 'held draft', 'ready'); c:poll()
    assert(c.stage == 'reviewing')
    return env.groups[#env.groups]
end
local function deliver(c, raw, fields)
    local child = {ok = true, text = raw}
    for k, v in pairs(fields or {}) do child[k] = v end
    env.groups[#env.groups].result = {done = true, result = {ok = child.ok, results = {child}}}
    c:poll()
end
local function disposed(c)
    assert(c.disposed and not env.owner and env.status == nil)
    for _, s in ipairs(env.snapshots) do assert(s.releases == 1) end
    for _, h in ipairs(env.groups) do assert(h.released) end
    assert(env.finishes == env.starts)
    local n = #env.finals
    c:poll(); c:dispose()
    assert(#env.finals == n and env.finishes == env.starts)
end
local function blocked(c, reason)
    assert(#env.finals == 1 and env.finals[1].kind == 'blocked')
    if reason then assert(env.finals[1].reason:find(reason, 1, true), env.finals[1].reason) end
    disposed(c)
end
local tests = {}
function tests.submission_identity_and_state()
    local c=setup()
    c.context.process_owner='identity-test'; c.context.request_id='request-1'
    c:attempt('first draft','ready')
    assert(controller.state('identity-test','/workspace')[1].stage=='queued')
    assert(c:attempt('duplicate draft','ready')==c.id and c.draft=='first draft')
    c:poll()
    assert(controller.state('identity-test','/workspace')[1].stage=='reviewing')
    assert(c:attempt('duplicate','ready')==c.id and #env.groups==1)
    local function duplicate(request, owner)
        local ctx={}
        for k,v in pairs(c.context) do ctx[k]=v end
        ctx.request_id=request; ctx.process_owner=owner or 'identity-test'
        return controller.new(c.config,ctx)
    end
    local other=duplicate('request-1')
    assert(other:attempt('retry','ready')==c.id and other.disposed)
    assert(#env.groups==1)
    deliver(c,clean)
    local state=controller.state('identity-test','/workspace')
    assert(state[1].stage=='finalized' and state[1].result:find('clean',1,true))
    assert(#controller.state('another-session','/workspace')==0)
    other=duplicate('request-1')
    assert(other:attempt('retry completed','ready')==c.id and other.disposed)
    for _,case in ipairs({{'request-2','identity-test'}, {'request-1','other-owner'}}) do
        other=duplicate(case[1],case[2]); other:attempt('new request','ready')
        assert(other.stage=='queued'); other:dispose()
    end
    other=duplicate('request-1') -- starts on v1, writes before ready
    env.version='v2'
    other:attempt('changed snapshot','ready')
    assert(other.stage=='queued'); other:dispose()
    other=duplicate('request-1') -- starts on v2, restores v1 before ready
    env.version='v1'
    assert(other:attempt('restored snapshot','ready')==c.id and other.disposed)
end
function tests.runtime_cleanup_state()
    for _,stage in ipairs({'queued','reviewing','parent_fixes'}) do
        for _,cancelled in ipairs({true,false}) do
            local c=setup()
            local owner='cleanup-'..stage..tostring(cancelled)
            c.context.process_owner=owner; c.context.request_id='request'
            c:attempt('draft','ready'); c.stage=stage
            c:dispose({cancelled=cancelled,error=cancelled and 'cancelled' or 'transport failure'})
            local state=controller.state(owner,'/workspace')[1]
            assert(state.stage==(cancelled and 'cancelled' or 'failed'))
            assert(state.result==(cancelled and 'cancelled' or 'transport failure'))
            disposed(c)
        end
    end
end
function tests.concurrent_input()
    local c=setup(); queue(c)
    assert(env.waiting and env.owner==nil, 'review must not hold write barrier')
    env.foreground_lost=true
    deliver(c,report('findings',{finding()}))
    blocked(c,'automatic repairs deferred')
    assert(c.stage=='repairs_deferred' and issues.read().runs[1].reason=='repairs_deferred')
    assert(#env.resumes==0 and #issues.read().issues==1)
    assert(not env.finals[1].text:find('held draft',1,true))
    assert(not env.finals[1].text:find('Review incomplete',1,true))
    assert(not env.finals[1].text:find('Workspace changed',1,true))
end
function tests.concurrent_input_clean()
    for _,phase in ipairs({'queued','reviewing'}) do
        local c=setup()
        c:attempt('held draft','ready')
        if phase=='reviewing' then c:poll() end
        env.foreground_lost=true
        c:poll()
        assert(c.stage=='reviewing')
        deliver(c,clean)
        assert(env.finals[1].kind=='ready' and env.finals[1].text:find('Review '..c.id..' completed:',1,true))
        assert(issues.read().runs[1].reason=='accepted' and #env.resumes==0)
        disposed(c)
    end
end
function tests.partial_read()
    local c=setup(); queue(c)
    c.snapshot.access_complete=function() return false end
    deliver(c,clean)
    blocked(c,'unread pages')
end
function tests.turn_limit_preserves_unread_detail()
    local c=setup(); queue(c)
    c.snapshot.access_complete=function() return false, '"large.lua" offset=48000' end
    deliver(c,'',{ok=false,error='max agent turns exceeded: 80'})
    blocked(c,'max agent turns exceeded: 80')
    assert(env.finals[1].reason:find('"large.lua" offset=48000',1,true))
end
function tests.clean()
    local c = setup(); queue(c)
    c:poll(); assert(#env.finals == 0)
    deliver(c, clean)
    assert(#env.finals == 1 and env.finals[1].text == 'Review '..c.id..' completed: Review evidence unavailable or checked\nNo open findings.' and env.finals[1].kind == 'ready')
    assert(#env.resumes == 0 and #env.groups == 1 and c.requests == 0)
    assert(issues.read().runs[1].reason == 'accepted' and env.telemetry.ok)
    disposed(c)
end
function tests.large_input_preserved()
    for _, unit in ipairs({'a', 'Я', '\"\\\n'}) do
        local c = setup()
        local text = string.rep(unit, 600 * 1024)
        c.context.messages = json.array({{role = 'user', content = text}})
        assert(tasks.update({revision = 0, tasks = {{id = 'large_review',
            title = 'Review complete input', status = 'completed', result = 'Verified'}}}))
        local h = queue(c)
        assert(#h.args.tasks[1].task > 512 * 1024)
        local input = json.decode(h.args.tasks[1].task)
        assert(input.task[1].content == text)
        assert(input.draft == 'held draft' and input.files[1] == 'src/example.c')
        assert(input.changes[1] == 'src/example.c' and #input.open_issues == 0)
        assert(input.task_plan.tasks[1].id == 'large_review')
        assert(input.task_plan.tasks[1].status == 'completed')
        assert(h.args.tasks[1].max_turns == 6 and c.requests == 0)
        deliver(c, clean)
        assert(env.finals[1].kind == 'ready'); disposed(c)
    end
end
function tests.large_input_provider_failure()
    local c = setup()
    c.context.messages = json.array({{role = 'user', content = string.rep('x', 600 * 1024)}})
    queue(c)
    deliver(c, '', {ok = false, error = 'provider context limit exceeded'})
    blocked(c, 'provider context limit exceeded')
    assert(issues.read().runs[1].reason ~= 'accepted' and #env.resumes == 0)
end
function tests.task_plan_refresh_and_issue()
    local c = setup()
    local entries = {}
    for _, status in ipairs({'pending', 'in_progress', 'blocked', 'completed', 'cancelled'}) do
        entries[#entries + 1] = {id = status, title = 'Task ' .. status, status = status,
            criteria = 'Required acceptance evidence', result = 'Result or reason ' .. status}
    end
    assert(tasks.update({revision = 0, tasks = entries}))
    local h = queue(c)
    local input = json.decode(h.args.tasks[1].task)
    assert(input.task_plan.revision == 1 and #input.task_plan.tasks == 5)
    for i, item in ipairs(entries) do
        local got = input.task_plan.tasks[i]
        for key, value in pairs(item) do assert(got[key] == value) end
    end
    local issue = {severity = 'high', description = 'Requested tasks remain unfinished',
        evidence = 'Task pending is pending; Required acceptance evidence is missing'}
    deliver(c, report('findings', {issue}))
    assert(env.resumes[1].issues[1].finding.evidence == issue.evidence)
    entries[1].status = 'completed'; entries[1].result = 'Verified acceptance evidence'
    assert(tasks.update({revision = 1, tasks = {entries[1]}}))
    h = queue(c)
    input = json.decode(h.args.tasks[1].task)
    assert(input.task_plan.revision == 2 and input.task_plan.tasks[1].status == 'completed')
    assert(input.task_plan.tasks[1].result == 'Verified acceptance evidence')
    issue.id = 'issue-1'
    deliver(c, report('findings', {issue}))
    -- Task-only progress must not be mistaken for unchanged files/no progress.
    assert(c.stage == 'parent_fixes' and c.cycles == 2)
    queue(c)
    deliver(c, report('clean', nil, {{id = 'issue-1', status = 'resolved', evidence = 'Verified tasks'}}))
    assert(env.finals[1].kind == 'ready'); disposed(c)
end
function tests.empty_and_invalid_task_plans()
    local c = setup(); local h = queue(c)
    local input = json.decode(h.args.tasks[1].task)
    assert(input.task_plan.revision == 0 and #input.task_plan.tasks == 0)
    assert(h.args.tasks[1].task:find('"tasks":[]', 1, true))
    deliver(c, clean); disposed(c)
    c = setup(); env.task_store.json = '{invalid'
    c:attempt('held draft', 'ready'); c:poll()
    blocked(c, 'Stored task plan is invalid'); assert(#env.groups == 0)
    assert(env.task_store.json == '{invalid')
end
function tests.zero_repairs()
    local c = setup({max_fix_cycles = 0}); queue(c); deliver(c, report('findings', {finding()}))
    blocked(c, 'maximum fix cycles'); assert(#env.resumes == 0)
    assert(env.finals[1].text:find('issue-1', 1, true) and issues.read().issues[1].status == 'open')
end
function tests.two_repairs_and_rechecks()
    local c = setup(); queue(c); deliver(c, report('findings', {finding()}))
    for cycle = 1, 2 do
        assert(c.stage == 'parent_fixes' and c.cycles == cycle and #env.finals == 0 and not env.owner)
        local data = env.resumes[cycle]
        assert(data.issues[1].id == 'issue-1')
        assert(issues.respond({id = 'issue-1', run_id = c.id, revision = data.revision,
            snapshot = data.snapshot, status = 'pending_verification', response = 'Fixed with evidence'}))
        env.version = 'v' .. (cycle + 1)
        local h = queue(c, 'repaired draft ' .. cycle)
        local input = json.decode(h.args.tasks[1].task)
        assert(input.cycle == cycle and #input.open_issues == 1)
        assert(input.open_issues[1].response == 'Fixed with evidence')
        if cycle == 1 then deliver(c, report('findings', {finding('issue-1')}))
        else deliver(c, report('clean', nil, {{id = 'issue-1', status = 'resolved', evidence = 'Verified fix'}})) end
    end
    assert(#env.resumes == 2 and #env.groups == 3)
    assert(issues.read().issues[1].status == 'resolved'); disposed(c)
end
function tests.two_repairs_exhausted()
    local c = setup(); queue(c); deliver(c, report('findings', {finding()}))
    for cycle = 1, 2 do
        env.version = 'changed-' .. cycle; queue(c, 'new draft'); deliver(c, report('findings', {finding('issue-1')}))
    end
    blocked(c, 'maximum fix cycles'); assert(#env.resumes == 2)
    assert(not env.finals[1].text:find('held draft', 1, true))
end
function tests.same_version_recheck_clean()
    local c = setup(); queue(c); deliver(c, report('findings', {finding()}))
    queue(c, 'verified draft')
    deliver(c, report('clean', nil, {{id = 'issue-1', status = 'resolved', evidence = 'New evidence, same source'}}))
    assert(env.finals[1].kind == 'ready')
    assert(#issues.read().runs[1].reports == 2); disposed(c)
end
function tests.no_progress()
    local c = setup(); queue(c); deliver(c, report('findings', {finding()}))
    queue(c); deliver(c, report('findings', {finding('issue-1')}))
    blocked(c, 'no progress'); assert(#env.resumes == 1)
end
function tests.invalid_results()
    for _, case in ipairs({{raw = ''}, {raw = '{}'}, {raw = report('inconclusive')},
        {raw = clean, fields = {text_truncated = true}}, {raw = clean, fields = {ok = false, error = 'provider failed'}}}) do
        local c = setup(); queue(c); deliver(c, case.raw, case.fields)
        blocked(c); assert(#env.resumes == 0)
    end
end
function tests.clean_after_access_error()
    local c = setup(); local h = queue(c)
    h.opts.reviewer.access_error = 'snapshot read exceeds output limit (65536 bytes)'
    deliver(c, clean)
    blocked(c, 'snapshot read exceeds output limit')
    assert(#env.resumes == 0 and not env.telemetry.ok)
    assert(issues.read().runs[1].reason ~= 'accepted')
end
function tests.enumeration_failure()
    for _, phase in ipairs({'capture', 'acceptance'}) do
        local c = setup()
        if phase == 'acceptance' then queue(c) end
        env.enumeration_error = 'snapshot enumeration failed'
        if phase == 'capture' then c:attempt('held draft', 'ready'); c:poll()
        else deliver(c, clean) end
        blocked(c, 'snapshot enumeration failed')
        assert(#env.resumes == 0 and not env.telemetry.ok)
        if phase == 'capture' then assert(#env.groups == 0)
        else assert(issues.read().runs[1].reason ~= 'accepted') end
    end
end
function tests.save_failures()
    for _, phase in ipairs({'begin', 'record', 'finish'}) do
        local c = setup()
        -- Real native store adapter allows fault injection without mocking issue policy.
        issues.use_store(nil)
        local raw, writes = '', 0
        local fail_at = ({begin = 1, record = 2, finish = 3})[phase]
        agent = {issues_get = function() return raw, 1 end,
            issues_set = function(next_raw, token)
                assert(token == 1); writes = writes + 1
                if writes >= fail_at then return false end
                raw = next_raw; return true
            end}
        c:attempt('held draft', 'ready'); c:poll()
        if phase ~= 'begin' then deliver(c, clean) end
        blocked(c, 'Could not save issues'); assert(#env.resumes == 0)
        agent = nil
    end
end
function tests.timeout_without_http()
    for _, stage in ipairs({'queued', 'reviewing', 'parent_fixes'}) do
        local c = setup()
        if stage == 'queued' then env.busy = true; c:attempt('held draft', 'ready'); c:poll()
        else queue(c); if stage == 'parent_fixes' then deliver(c, report('findings', {finding()})) end end
        assert(c.stage == stage)
        env.clock = 10000; c:poll(); blocked(c, 'deadline exceeded')
    end
end
function tests.cancellation()
    for _, stage in ipairs({'queued', 'reviewing', 'parent_fixes'}) do
        local c = setup()
        if stage == 'queued' then c:attempt('held draft', 'ready')
        else queue(c); if stage == 'parent_fixes' then deliver(c, report('findings', {finding()})) end end
        env.cancelled = true; c:poll()
        assert(c.stage == 'cancelled' and env.cancels == 1 and #env.finals == 0)
        disposed(c); assert(env.cancels == 1)
        if stage == 'reviewing' then assert(env.groups[1].cancelled) end
    end
end
function tests.drift()
    for _, raw in ipairs({clean, report('findings', {finding()})}) do
        local c = setup(); queue(c); env.drift = true; deliver(c, raw)
        assert(c.stage == 'stale'); blocked(c, 'external workspace drift')
        assert(#env.resumes == 0 and env.finals[1].text:find('Please confirm', 1, true))
    end
end
function tests.shared_request_budget()
    local c = setup({max_requests = 2})
    assert(c:consume() and c.requests == 0) -- ordinary execution is outside review budget
    local h = queue(c)
    assert(h.opts.reviewer.budget:consume()) -- reviewer request
    deliver(c, report('findings', {finding()}))
    assert(c:consume() and c.requests == 2) -- parent repair / retry uses same owner
    local ok, err = c:consume(); assert(not ok and err:find('request budget exhausted', 1, true))
    assert(c.requests == 2)
    c:stop(err); blocked(c, 'request budget exhausted')
    assert(env.telemetry.attrs.request_count == 2)
end
-- Real snapshot -> dispatcher -> controller path; only native filesystem and
-- scheduler transport are mocked. No live file adapters or permission prompts.
local real_snapshots = dofile('agent/review_snapshot.lua')
local controller_telemetry = package.loaded['agent.telemetry']
package.loaded['agent.telemetry'] = {
    start=function() return {} end, finish=function() end,
    tool_attributes=function() return {} end,
}
local dispatch = require('agent.tools')
package.loaded['agent.telemetry'] = controller_telemetry
local function reading_controller(files)
    local c=setup(); queue(c)
    capstan.realpath=function(path) return path end
    tools={
        review_snapshot_list=function()
            local entries={}
            for path in pairs(files) do entries[#entries+1]={path=path,kind='file',ignored=false} end
            return entries
        end,
        review_snapshot_read=function(_,path) return files[path] end,
    }
    local snap=assert(real_snapshots.capture('/workspace',function() return true end))
    c.snapshot:release(); c.snapshot=snap; c.reviewer.snapshot=snap
    local function read(args)
        local response, event
        dispatch.handle_tool_calls({}, {env.tool}, {{id='read',name='file_read',arguments=json.encode(args)}}, '',
            function(messages) response=messages[#messages].content end,
            {reviewer=c.reviewer,silent_tools=true,tools={env.tool},callbacks={
                on_tool_end=function(value) event=value end,
            }})
        assert(type(response)=='string')
        return response,event
    end
    return c,read
end
function tests.arguments_retry_dispatch()
    local c,read=reading_controller({a='aaa',b='bbb',c='ccc'})
    local response=read({paths={'a','b','c'},limit=48000})
    assert(response:find('retry required',1,true) and not response:find('access denied',1,true))
    assert(not c.reviewer.access_error)
    for _,args in ipairs({{path='a',offset=-1},{path='a',limit=3},
        {path='a',offset=0.5},{path='a',offset=false},{path='a',limit=false},
        {path='a',limit=48001},{path='a',offset=100}}) do
        assert(read(args):find('retry required',1,true))
        assert(not c.reviewer.access_error)
    end
    assert(read({paths={'a','b','c'}}):find('c\nccc',1,true))
    deliver(c,clean); assert(env.finals[1].kind=='ready'); disposed(c)
end
function tests.oversized_retry_dispatch()
    local c,read=reading_controller({a=string.rep('a',50000),b=string.rep('b',50000)})
    assert(read({paths={'a','b'}}):find('retry required',1,true))
    assert(not c.reviewer.access_error and not c.snapshot:access_complete())
    for _,path in ipairs({'a','b'}) do
        read({path=path,offset=0,limit=48000})
        read({path=path,offset=48000,limit=48000})
    end
    read({path='a',offset=0,limit=4}) -- completed coverage must not regress
    assert(c.snapshot:access_complete())
    deliver(c,clean); assert(env.finals[1].kind=='ready'); disposed(c)
end
function tests.snapshot_delivery_not_truncated()
    for _,configured in ipairs({false,true}) do
        local content=string.rep('x\n',24000)
        local c,read=reading_controller({a=content,b=string.rep('y',60000)})
        if configured then capstan.config={tool_output={max_lines=1,max_bytes=1024}} end
        local page=read({path='a',offset=0,limit=48000})
        assert(page:match('^[^\n]*\n(.*)$')==content)
        assert(read({path='b'})=='b\n'..string.rep('y',60000))
        assert(c.snapshot:access_complete() and not c.reviewer.access_error)
        deliver(c,clean); assert(env.finals[1].kind=='ready'); disposed(c)
    end
end
function tests.invalid_utf8_delivery_blocks_acceptance()
    local c,read=reading_controller({a='x'..string.char(255)})
    read({path='a'})
    deliver(c,clean); blocked(c,'invalid UTF-8 bytes replaced')
end
function tests.unread_batch_dispatch()
    local c,read=reading_controller({a=string.rep('a',50000),b=string.rep('b',50000)})
    read({paths={'a','b'}}); read({path='a'}); read({path='b',offset=0,limit=4})
    deliver(c,clean); blocked(c,'"b" offset=4')
end
function tests.denial_sticky_dispatch()
    local c,read=reading_controller({a='allowed'})
    assert(read({path='../outside'}):find('access denied',1,true))
    local fatal=assert(c.reviewer.access_error)
    read({path='a',limit=3}); read({path='a'})
    assert(c.reviewer.access_error==fatal)
    deliver(c,clean); blocked(c,'path unavailable')
end
local names = {}; for name in pairs(tests) do names[#names + 1] = name end; table.sort(names)
local failures = 0
for _, name in ipairs(names) do
    local ok, err = pcall(tests[name])
    if ok then print('completion_review/' .. name .. ': passed')
    else failures = failures + 1; io.stderr:write('completion_review/' .. name .. ': ' .. tostring(err) .. '\n') end
end
issues.use_store(nil)
tasks.use_store(nil)
assert(failures == 0, tostring(failures) .. ' completion review test(s) failed')
print('completion_review: ' .. #names .. ' focused controller tests passed')
