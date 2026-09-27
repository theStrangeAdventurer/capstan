local issues=require('agent.issues')
local verdict=require('agent.review_verdict')
local json=require('vendor.rxi.json')
local function equal(a,b)
    if type(a) ~= type(b) then return false end
    if type(a) ~= 'table' then return a == b end
    for k,v in pairs(a) do if not equal(v,b[k]) then return false end end
    for k,v in pairs(b) do if a[k] == nil then return false end end
    return true
end
local function array(t) return json.array(t or {}) end
local function report(kind,findings,checks)
    return json.encode({verdict=kind,summary='Review evidence',findings=array(findings),checks=array(checks)})
end
local function finding(id)
    return {id=id,severity='high',description='Пропущена проверка ошибки',evidence='A failed save returns success',
        file='src/example.c',start_line=10,end_line=12}
end
local clean=report('clean')
assert(verdict.parse(clean))
assert(verdict.parse(report('inconclusive')))
for _, raw in ipairs({'', 'clean', '{}', 'null', clean..'extra', '```json\n'..clean..'\n```',
    '{"verdict":"clean","verdict":"clean","summary":"x","findings":[],"checks":[]}',
    '{"verdict":"clean","summary":"x","findings":[null],"checks":[]}',
    '{"verdict":"clean","summary":"x","findings":{},"checks":[]}',
    '{"verdict":"clean","summary":"x","findings":[],"checks":[],}',
    '{"verdict":"clean","summary":"x","findings":[,],"checks":[]}',
    '{"verdict":"clean","summary":"x","findings":[],"checks":[],"extra":1}',
    report('clean',{finding()}),report('findings'), report('unknown'),
    report('findings',{finding('one'),finding('one')}),
    report('clean',nil,{{id='one',status='open',evidence='still broken'}}),
    report('inconclusive',nil,{{id='one',status='resolved',evidence='guess'}}),
    string.rep(' ',128*1024+1)}) do
    assert(not verdict.parse(raw),raw)
end
for _, change in ipairs({{severity='cosmetic'},{evidence=''},{start_line=0},{end_line=9},
    {description='a\0b'},{evidence=string.rep('x',4097)}}) do
    local item=finding(); for k,v in pairs(change) do item[k]=v end
    assert(not verdict.parse(report('findings',{item})))
end
-- Location is optional for missing implementations; evidence is not.
assert(verdict.parse(report('findings',{{severity='medium',description='Missing requested command',evidence='No handler'}})))
-- Stale rechecks close a pre-existing issue without a new finding.
assert(verdict.parse(report('clean',nil,{{id='one',status='stale',evidence='Context no longer exists'}})))
local store={json=''}; issues.use_store(store)
assert(issues.read().revision == 0 and issues.display() == 'No review issues.')
local handle,ledger=issues.begin('run-1','baseline-1','snapshot-1',0)
assert(handle and ledger.revision == 1)
assert(not issues.begin('run-1','b','s',1))
local initial=store.json
for _, metadata in ipairs({{}, {ok=false,truncated=false},{ok=true},{ok=true,truncated=true}}) do
    assert(not issues.record(handle,1,'snapshot-1',clean,metadata))
    assert(store.json == initial)
end
local transport={ok=true,truncated=false}
assert(not issues.record(handle,0,'snapshot-1',clean,transport))
assert(not issues.record(handle,1,'wrong',clean,transport))
assert(not issues.record({},1,'snapshot-1',clean,transport))
assert(store.json == initial)
ledger=assert(issues.record(handle,1,'snapshot-1',report('findings',{finding()}),transport))
local issue=ledger.issues[1]
assert(issue.id == 'issue-1' and issue.status == 'open' and issue.original_snapshot == 'snapshot-1')
assert(ledger.revision == 2 and #ledger.runs[1].reports == 1)
assert(issues.display(issue.id):find('Пропущена',1,true))
local saved=store.json
assert(not issues.record(handle,2,'snapshot-1',clean,transport)) -- duplicate snapshot
local args={revision=2,id=issue.id,run_id='run-1',snapshot='snapshot-1',response='Fixed and checked'}
for _, status in ipairs({'resolved','accepted_risk','deleted','open'}) do
    args.status=status; assert(not issues.respond(args))
end
args.status='pending_verification'; args.run_id='other'
assert(not issues.respond(args)); args.run_id='run-1'; args.snapshot='stale'
assert(not issues.respond(args)); args.snapshot='snapshot-1'
assert(store.json == saved)
ledger=assert(issues.respond(args))
assert(ledger.issues[1].status == 'pending_verification')
ledger=assert(issues.snapshot(handle,3,'snapshot-1','snapshot-2'))
saved=store.json
assert(not issues.record(handle,4,'snapshot-1',clean,transport))
assert(not issues.record(handle,4,'snapshot-2',clean,transport)) -- no silent disappearance
assert(not issues.record(handle,4,'snapshot-2',report('clean',nil,{{id='alien',status='resolved',evidence='x'}}),transport))
assert(store.json == saved)
-- Inconclusive preserves open issues and is explicitly recorded as such.
ledger=assert(issues.record(handle,4,'snapshot-2',report('inconclusive'),transport))
assert(ledger.issues[1].status == 'pending_verification')
assert(issues.snapshot(handle,5,'snapshot-2','snapshot-3'))
ledger=assert(issues.record(handle,6,'snapshot-3',report('findings',{finding('issue-1')}),transport))
assert(#ledger.issues == 1 and ledger.issues[1].status == 'open')
assert(ledger.issues[1].response == 'Fixed and checked' and ledger.issues[1].snapshot == 'snapshot-3')
args.revision,args.snapshot,args.status,args.response=7,'snapshot-3','disputed','This behavior is required'
assert(issues.respond(args))
assert(issues.snapshot(handle,8,'snapshot-3','snapshot-4'))
ledger=assert(issues.record(handle,9,'snapshot-4',report('clean',nil,{
    {id='issue-1',status='resolved',evidence='Verified source and requirement'},
}),transport))
assert(ledger.issues[1].status == 'resolved' and ledger.issues[1].original_snapshot == 'snapshot-1')
assert(issues.finish(handle,10,'snapshot-4','clean'))
assert(not issues.record(handle,11,'snapshot-4',clean,transport))
assert(not issues.finish(handle,11,'snapshot-4','again'))
-- Returned values never alias persisted records; history is independent of tasks.
ledger.issues[1].status='open'
assert(issues.read().issues[1].status == 'resolved')
local second={json=''}; issues.use_store(second)
local other=assert(issues.begin('run-2','unknown-baseline','v1',0))
assert(issues.record(other,1,'v1',report('findings',{finding()}),transport))
local plugin=dofile('plugins/issues.lua')
local ctx={args={}}
function ctx:replace(ui,llm) return ui,llm end
local ui,llm=plugin.handler(ctx); assert(ui:find('issue-1',1,true) and llm == '')
ctx.tool_args={operation='accept',id='issue-1'}
local _,_,ok=plugin.handler(ctx); assert(ok == false)
ctx.tool_args=nil; ctx.args={'accept','issue-1'}
assert(plugin.handler(ctx):find('Usage:',1,true))
ctx.args={'accept','issue-1','Explicitly','accepted'}
ui,llm=plugin.handler(ctx); assert(ui:find('accepted_risk',1,true) and llm == '')
assert(issues.read().issues[1].risk_reason == 'Explicitly accepted')
-- Handles bind their ACP store, not whichever session is selected at delivery.
issues.use_store(store)
local before=store.json
assert(issues.snapshot(other,3,'v1','v2'))
assert(issues.record(other,4,'v2',clean,transport)) -- accepted risk remains explicit
assert(store.json == before and json.decode(second.json).issues[1].status == 'accepted_risk')
issues.release(other)
assert(not issues.snapshot(other,5,'v2','v3'))
-- Corrupt stores, exhaustion and malformed reports never discard prior state.
issues.use_store(second)
second.json='{broken'
assert(not issues.read()); assert(not issues.begin('new','b','s',0)); assert(second.json == '{broken')
second.json=''
local h=assert(issues.begin('limit','b','s',0))
local too_many={}; for i=1,101 do too_many[i]=finding() end
before=second.json
assert(not issues.record(h,1,'s',report('findings',too_many),transport)); assert(second.json == before)
-- Native generation capture, storage failures and exceptions fail closed.
local original_agent=agent
local native_raw,token='',1
agent={issues_get=function() return native_raw,token end,
    issues_set=function(raw,expected) if expected ~= token then return false end; native_raw=raw; return true end}
issues.use_store(nil)
h=assert(issues.begin('native','b','s',0)); before=native_raw
agent.issues_set=function() return false end
assert(not issues.record(h,1,'s',clean,transport)); assert(native_raw == before)
agent.issues_set=function() error('disk error') end
assert(not issues.record(h,1,'s',clean,transport)); assert(native_raw == before)
token=2
assert(not issues.record(h,1,'s',clean,transport)); assert(native_raw == before)
agent=original_agent
issues.use_store(nil)
print('issues: protocol, revisions, rechecks, risk, isolation and failure tests passed')

-- Archive complete closed runs only; normal reads stay bounded.
for _, resolved in ipairs({false,true}) do
    local archive_store={json=''}
    issues.use_store(archive_store)
    local revision=0
    for i=1,300 do
        local handle=assert(issues.begin('cycle-'..i,'b','s',revision))
        revision=revision+1
        local result=assert(issues.record(handle,revision,'s',resolved and report('findings',{finding()}) or clean,transport))
        revision=result.revision
        if resolved then
            result=assert(issues.snapshot(handle,revision,'s','s2'))
            result=assert(issues.record(handle,result.revision,'s2',report('clean',nil,{{id='issue-'..i,status='resolved',evidence='Verified'}}),transport))
            revision=result.revision
        end
        result=assert(issues.finish(handle,revision,resolved and 's2' or 's','done'))
        revision=result.revision
        assert(#result.runs<=100 and #result.issues<=100 and #json.encode(result)<=256*1024)
    end
    local active=issues.read()
    local history=issues.history()
    assert(#active.runs+#history.runs==300 and #history.runs>=200)
    assert(#history.runs[1].reports==(resolved and 2 or 1))
    assert(not issues.begin('cycle-1','b','s',revision))
    assert(not issues.begin('new','b','s',revision-1))
    if resolved then assert(active.next_id==301 and #active.issues+#history.issues==300) end
    local raw=archive_store.json
    issues.use_store({json=raw}) -- persisted envelope reload
    assert(#issues.history().runs==#history.runs)
    ctx.args={'history','cycle-1'}
    local text,context=plugin.handler(ctx)
    assert(text:find('Review evidence',1,true) and context=='')
end
issues.use_store(nil)
print('issues: 300 clean and resolved archive cycles passed')

-- Closed runs containing unresolved issues remain pinned under byte pressure.
local pinned={json=''}; issues.use_store(pinned)
local ph=assert(issues.begin('pinned','b','s',0))
local pl=assert(issues.record(ph,1,'s',report('findings',{finding()}),transport))
pl=assert(issues.finish(ph,pl.revision,'s','blocked'))
local big={}; for i=1,10 do
    big[i]={severity='high',description=string.rep('d',2000),evidence=string.rep('e',4000)}
end
for i=1,4 do
    local bh=assert(issues.begin('bytes-'..i,'b','s',pl.revision))
    pl=assert(issues.record(bh,pl.revision+1,'s',report('findings',big),transport))
    local checks={}
    for _, item in ipairs(pl.issues) do
        if item.run_id=='bytes-'..i then checks[#checks+1]={id=item.id,status='resolved',evidence='checked'} end
    end
    pl=assert(issues.snapshot(bh,pl.revision,'s','s2'))
    pl=assert(issues.record(bh,pl.revision,'s2',report('clean',nil,checks),transport))
    pl=assert(issues.finish(bh,pl.revision,'s2','done'))
end
assert(#issues.history().runs>0 and pl.issues[1].id=='issue-1' and pl.issues[1].status=='open')
assert(#pl.runs<100 and #pl.issues<100 and #json.encode(pl)<=256*1024)
local preserved=pinned.json
local native_archive=pinned.json
agent={issues_get=function() return native_archive,1 end,issues_set=function() return false end}
issues.use_store(nil)
assert(not issues.begin('save-failure','b','s',pl.revision))
assert(native_archive==preserved)
agent=original_agent
print('issues: byte-pressure pinning and archived-save rollback passed')

-- Persisted active state must still obey the byte limit, even with archival.
local oversized_active=json.decode(preserved)
local extra=oversized_active.archive.runs[1]
extra=json.decode(json.encode(extra))
extra.id='oversized-stored-run'
for i=1,32 do
    extra.reports[i]={snapshot='version-'..i,report={verdict='clean',
        summary=string.rep('x',4096),findings=array(),checks=array()}}
end
table.insert(oversized_active.runs,extra)
local another=json.decode(json.encode(extra)); another.id='another-oversized-run'
table.insert(oversized_active.runs,another)
local oversized_store={json=json.encode(oversized_active)}
issues.use_store(oversized_store)
local oversized_before=oversized_store.json
assert(not issues.read())
assert(not issues.begin('must-not-repair-invalid-store','b','s',oversized_active.revision))
assert(oversized_store.json==oversized_before)
issues.use_store(nil)

-- Issue count alone evicts a whole eligible run, including accepted risk.
local count_store={json=''}; issues.use_store(count_store)
local ch,cl=issues.begin('count-old','b','s',0); assert(ch)
local fifty={}; for i=1,50 do fifty[i]=finding() end
cl=assert(issues.record(ch,cl.revision,'s',report('findings',fifty),transport))
cl=assert(issues.accept_risk({revision=cl.revision,id='issue-1',run_id='count-old',
    snapshot='s',response='Owner accepts this specific risk'}))
local accepted=json.encode(cl.issues[1])
local checks={}; for i=2,50 do checks[#checks+1]={id='issue-'..i,status='resolved',evidence='Verified'} end
cl=assert(issues.snapshot(ch,cl.revision,'s','s2'))
cl=assert(issues.record(ch,cl.revision,'s2',report('clean',nil,checks),transport))
cl=assert(issues.finish(ch,cl.revision,'s2','done'))
ch,cl=issues.begin('count-new','b','s',cl.revision); assert(ch)
cl=assert(issues.record(ch,cl.revision,'s',report('findings',fifty),transport))
assert(#cl.issues==100 and #cl.runs==2 and #issues.history().runs==0)
cl=assert(issues.snapshot(ch,cl.revision,'s','s2'))
-- Even the entire stored envelope after eviction is below the byte limit.
local rechecks={}; for i=51,100 do rechecks[#rechecks+1]={id='issue-'..i,status='open',evidence='Still open'} end
cl=assert(issues.record(ch,cl.revision,'s2',report('findings',{finding()},rechecks),transport))
assert(#count_store.json<256*1024 and #cl.runs==1 and #cl.issues==51)
local archived=issues.history()
assert(#archived.runs==1 and archived.runs[1].id=='count-old' and #archived.issues==50)
assert(equal(archived.issues[1], json.decode(accepted)))
issues.use_store({json=count_store.json})
assert(equal(issues.history().issues[1], json.decode(accepted)))

-- Every unresolved status pins closed runs; exhaustion changes no persisted byte.
for _, status in ipairs({'open','pending_verification','disputed'}) do
    local blocked={json=''}; issues.use_store(blocked)
    local bh,bl=issues.begin('blocked','b','s',0); assert(bh)
    local hundred={}; for i=1,100 do hundred[i]=finding() end
    bl=assert(issues.record(bh,bl.revision,'s',report('findings',hundred),transport))
    -- One unresolved issue suffices to pin the entire run.
    local resolved_checks={}
    for i=2,100 do resolved_checks[#resolved_checks+1]={id='issue-'..i,status='resolved',evidence='Verified'} end
    bl=assert(issues.snapshot(bh,bl.revision,'s','s2'))
    bl=assert(issues.record(bh,bl.revision,'s2',report('findings',{finding('issue-1')},resolved_checks),transport))
    if status~='open' then
        bl=assert(issues.respond({revision=bl.revision,id='issue-1',run_id='blocked',
            snapshot='s2',status=status,response='Requires reviewer verification'}))
    end
    bl=assert(issues.finish(bh,bl.revision,'s2','blocked'))
    bh,bl=issues.begin('overflow','b','s',bl.revision); assert(bh)
    local unchanged=blocked.json
    assert(#bl.runs<100 and #unchanged+4096<256*1024)
    local result,err=issues.record(bh,bl.revision,'s',report('findings',{finding()}),transport)
    assert(not result and err:find('active limits reached',1,true))
    assert(blocked.json==unchanged and #issues.history().runs==0)
    assert(issues.read().issues[1].status==status and issues.read().next_id==101)
    -- The failed report did not consume its revision, snapshot or handle.
    assert(issues.record(bh,bl.revision,'s',clean,transport))
end
issues.use_store(nil)
print('issues: count-only eviction, archived risk and all pinned statuses passed')

-- Explicit global recheck set: record() only enforces cross-run open issues when asked.
local cross={json=''}; issues.use_store(cross)
local ah,al=issues.begin('run-a','b','s1',0)
al=assert(issues.record(ah,al.revision,'s1',report('findings',{finding()}),transport))
assert(al.issues[1].id=='issue-1' and al.issues[1].status=='open')
issues.release(ah)
local bh,bl=issues.begin('run-b','b','s2',al.revision)
-- Default enforcement stays scoped to the current run.
bl=assert(issues.record(bh,bl.revision,'s2',report('findings',{finding()}),transport))
assert(bl.issues[1].status=='open' and bl.issues[1].run_id=='run-a')
assert(bl.issues[2].id=='issue-2')
bl=assert(issues.snapshot(bh,bl.revision,'s2','s3'))
local revision=bl.revision
local bad,err=issues.record(bh,revision,'s3',report('findings',{finding()}),transport,{'issue-1'})
assert(not bad and err:find('Review omitted an open issue',1,true))
bl=assert(issues.record(bh,revision,'s3',report('findings',{finding('issue-1'),finding()}),transport,{'issue-1'}))
assert(bl.issues[1].id=='issue-1' and bl.issues[1].status=='open' and bl.issues[1].snapshot=='s3')
issues.release(bh)
issues.use_store(nil)
print('issues: explicit global recheck set passed')

