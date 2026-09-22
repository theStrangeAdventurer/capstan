-- Canonical issue policy. Executors can respond, never erase/accept their own
-- findings. Runtime report handles capture the store, run and baseline.
local json = require('vendor.rxi.json')
local verdict = require('agent.review_verdict')
local M = {}
local scoped_store
local handles = setmetatable({}, {__mode='k'})
local MAX_BYTES = 256 * 1024
local states = {open=true,pending_verification=true,disputed=true,resolved=true,accepted_risk=true}
local function copy(value) return json.decode(json.encode(value)) end
local function id(value) return verdict.text(value,64,true) and value:match('^[a-zA-Z0-9_-]+$') end
local function empty() return {revision=0,next_id=1,runs=json.array({}),issues=json.array({})} end
local function bind()
    if scoped_store then
        local store = scoped_store
        return {get=function() return store.json or '' end, set=function(raw) store.json=raw; return true end}
    end
    local native = rawget(_G,'agent')
    if not native or not native.issues_get or not native.issues_set then return nil, 'Issue storage is unavailable' end
    local _, token = native.issues_get()
    return {
        get=function()
            local raw, current = native.issues_get()
            if current ~= token then return nil, 'Issue session changed; stale callback rejected' end
            return raw
        end,
        set=function(raw) return native.issues_set(raw,token) end,
    }
end
function M.use_store(store) scoped_store=store end

local function unresolved(issue) return issue.status ~= 'resolved' and issue.status ~= 'accepted_risk' end
local function valid(ledger, unlimited)
    if type(ledger) ~= 'table' or not verdict.integer(ledger.revision,0,2147483646) or
        not verdict.integer(ledger.next_id,1,2147483646) or not verdict.array(ledger.runs,unlimited and math.huge or 100) or
        not verdict.array(ledger.issues,unlimited and math.huge or 100) then return false end
    local runs, ids = {}, {}
    for _, run in ipairs(ledger.runs) do
        if type(run) ~= 'table' or not id(run.id) or runs[run.id] or
            not verdict.text(run.baseline,128,true) or not verdict.text(run.snapshot,128,true) or
            not verdict.array(run.reports,32) or type(run.closed) ~= 'boolean' or
            not verdict.text(run.reason,4096,false) then return false end
        runs[run.id] = run
        for _, report in ipairs(run.reports) do
            if type(report) ~= 'table' or not verdict.text(report.snapshot,128,true) or
                not verdict.parse(json.encode(report.report)) then return false end
        end
    end
    for _, issue in ipairs(ledger.issues) do
        if type(issue) ~= 'table' or not id(issue.id) or ids[issue.id] or not runs[issue.run_id] or
            issue.baseline ~= runs[issue.run_id].baseline or not states[issue.status] or
            not verdict.text(issue.snapshot,128,true) or not verdict.text(issue.original_snapshot,128,true) or
            not verdict.finding(issue.finding) or issue.finding.id ~= nil or
            not verdict.text(issue.response,4096,false) or not verdict.text(issue.recheck,4096,false) or
            not verdict.text(issue.risk_reason,4096,false) then return false end
        ids[issue.id] = true
    end
    return true
end
local function active(ledger)
    return {revision=ledger.revision,next_id=ledger.next_id,runs=ledger.runs,issues=ledger.issues}
end
local function read(store)
    if not store then return nil, 'Issue storage is unavailable' end
    local ok, raw, err = pcall(store.get)
    if not ok then return nil, 'Could not read issues' end
    if raw == nil then return nil, err end
    if raw == '' then return empty() end
    local ledger = verdict.decode(raw,math.huge)
    local checked, good = pcall(function()
        if not valid(ledger) or #json.encode(active(ledger)) > MAX_BYTES or
            (ledger.archive ~= nil and type(ledger.archive) ~= 'table') then return false end
        local archive = ledger.archive or {runs=json.array({}),issues=json.array({})}
        if not valid({revision=ledger.revision,next_id=ledger.next_id,runs=archive.runs,issues=archive.issues},true) then return false end
        local combined = {revision=ledger.revision,next_id=ledger.next_id,runs=json.array({}),issues=json.array({})}
        for _, part in ipairs({archive,ledger}) do
            if not verdict.array(part.runs,math.huge) or not verdict.array(part.issues,math.huge) then return false end
            for _, run in ipairs(part.runs) do
                if part == archive and not run.closed then return false end
                table.insert(combined.runs,run)
            end
            for _, issue in ipairs(part.issues) do
                if part == archive and unresolved(issue) then return false end
                local number = tonumber(issue.id:match('^issue%-(%d+)$'))
                if not number or number >= ledger.next_id then return false end
                table.insert(combined.issues,issue)
            end
        end
        return valid(combined,true)
    end)
    if not checked or not good then return nil, 'Stored issues are invalid; previous state was preserved' end
    return ledger
end
local function save(store, ledger)
    if not valid(ledger,true) then return nil, 'Issue registry limits or schema violation' end
    local archive = ledger.archive or {runs=json.array({}),issues=json.array({})}
    while #ledger.runs > 100 or #ledger.issues > 100 or #json.encode(active(ledger)) > MAX_BYTES do
        local candidate
        for index, run in ipairs(ledger.runs) do
            local eligible = run.closed
            for _, issue in ipairs(ledger.issues) do
                if issue.run_id == run.id and unresolved(issue) then eligible=false; break end
            end
            if eligible then candidate=index; break end
        end
        if not candidate then return nil, 'Issue registry active limits reached; unresolved history was preserved' end
        local run=table.remove(ledger.runs,candidate)
        table.insert(archive.runs,run)
        local retained=json.array({})
        for _, issue in ipairs(ledger.issues) do
            table.insert(issue.run_id == run.id and archive.issues or retained,issue)
        end
        ledger.issues=retained
    end
    if #archive.runs > 0 then ledger.archive=archive end
    local raw = json.encode(ledger)
    local ok, saved = pcall(store.set,raw)
    if not ok or not saved then return nil, 'Could not save issues; acceptance is not confirmed' end
    return active(ledger)
end
local function current(store, revision)
    local ledger, err = read(store)
    if not ledger then return nil, err end
    if revision ~= ledger.revision then return nil, 'Issue revision conflict: read current issues first' end
    return ledger
end
function M.read(history)
    local store,err=bind(); if not store then return nil,err end
    local ledger; ledger,err=read(store); if not ledger then return nil,err end
    if history then return ledger.archive or {runs=json.array({}),issues=json.array({})} end
    return active(ledger)
end
function M.history() return M.read(true) end
local function run_for(ledger, run_id)
    for _, run in ipairs(ledger.runs) do if run.id == run_id then return run end end
end
local function live(handle, revision, snapshot)
    local owner = handles[handle]
    if not owner then return nil, 'Unknown or released issue report handle' end
    local ledger, err = current(owner.store,revision)
    if not ledger then return nil,err end
    local run = run_for(ledger,owner.run_id)
    if not run or run.closed or run.baseline ~= owner.baseline or run.snapshot ~= snapshot then
        return nil, 'Stale or closed review run/snapshot'
    end
    return ledger,run,owner.store
end
function M.begin(run_id, baseline, snapshot, revision)
    if not id(run_id) or not verdict.text(baseline,128,true) or not verdict.text(snapshot,128,true) then
        return nil, 'Invalid review identity'
    end
    local store, err = bind(); if not store then return nil,err end
    local ledger; ledger,err=current(store,revision); if not ledger then return nil,err end
    if run_for(ledger,run_id) or (ledger.archive and run_for(ledger.archive,run_id)) then return nil, 'Duplicate review run' end
    table.insert(ledger.runs,{id=run_id,baseline=baseline,snapshot=snapshot,closed=false,reason='',reports=json.array({})})
    ledger.revision=ledger.revision+1
    local saved; saved,err=save(store,ledger); if not saved then return nil,err end
    local handle={}
    handles[handle]={store=store,run_id=run_id,baseline=baseline}
    return handle,saved
end
function M.snapshot(handle, revision, previous, next_snapshot)
    local ledger,run,store=live(handle,revision,previous)
    if not ledger then return nil,run end
    if not verdict.text(next_snapshot,128,true) or next_snapshot == previous then return nil,'Require a new snapshot version' end
    run.snapshot=next_snapshot
    ledger.revision=ledger.revision+1
    return save(store,ledger)
end
-- Only the runtime may call this; never expose report publication as a model tool.
function M.record(handle, revision, snapshot, raw, transport)
    if type(transport) ~= 'table' or transport.ok ~= true or transport.truncated ~= false then
        return nil, 'Incomplete reviewer transport; not a clean verdict'
    end
    local report,err=verdict.parse(raw); if not report then return nil,err end
    local ledger,run,store=live(handle,revision,snapshot)
    if not ledger then return nil,run end
    for _, previous in ipairs(run.reports) do
        if previous.snapshot == snapshot then return nil,'Duplicate report for snapshot' end
    end
    local by_id,seen={},{}
    for _, issue in ipairs(ledger.issues) do
        if issue.run_id == run.id then by_id[issue.id]=issue end
    end
    for _, finding in ipairs(report.findings) do
        local issue=finding.id and by_id[finding.id]
        if finding.id and (not issue or not unresolved(issue)) then return nil,'Unknown or closed finding ID' end
        if not issue then
            local new_id='issue-'..ledger.next_id
            for _, existing in ipairs(ledger.issues) do
                if existing.id == new_id then return nil,'Issue ID sequence is corrupt' end
            end
            ledger.next_id=ledger.next_id+1
            issue={id=new_id,run_id=run.id,baseline=run.baseline,original_snapshot=snapshot,
                response='',recheck='',risk_reason=''}
            table.insert(ledger.issues,issue)
        end
        local item=copy(finding); item.id=nil
        issue.finding,issue.snapshot,issue.status,issue.recheck=item,snapshot,'open',finding.evidence
        seen[issue.id]=true
    end
    for _, check in ipairs(report.checks) do
        local issue=by_id[check.id]
        if not issue or not unresolved(issue) then return nil,'Unknown or closed recheck ID' end
        issue.status,issue.snapshot,issue.recheck=check.status,snapshot,check.evidence
        seen[issue.id]=true
    end
    if report.verdict ~= 'inconclusive' then
        for issue_id,issue in pairs(by_id) do
            if unresolved(issue) and not seen[issue_id] then return nil,'Review omitted an open issue' end
        end
    end
    table.insert(run.reports,{snapshot=snapshot,report=report})
    ledger.revision=ledger.revision+1
    return save(store,ledger)
end
function M.finish(handle, revision, snapshot, reason)
    local ledger,run,store=live(handle,revision,snapshot)
    if not ledger then return nil,run end
    if not verdict.text(reason,4096,true) then return nil,'A terminal reason is required' end
    run.closed,run.reason=true,reason
    ledger.revision=ledger.revision+1
    local saved,err=save(store,ledger)
    if saved then handles[handle]=nil end
    return saved,err
end
function M.release(handle) handles[handle]=nil end

local function respond(args, user)
    if type(args) ~= 'table' or not id(args.id) or not verdict.text(args.response,4096,true) then
        return nil,'Require issue ID and a nonblank response/reason'
    end
    if not user and args.status ~= 'pending_verification' and args.status ~= 'disputed' then
        return nil,'Executors may only mark pending_verification or disputed'
    end
    local store,err=bind(); if not store then return nil,err end
    local ledger; ledger,err=current(store,args.revision); if not ledger then return nil,err end
    for _, issue in ipairs(ledger.issues) do
        if issue.id == args.id then
            if issue.run_id ~= args.run_id or issue.snapshot ~= args.snapshot then return nil,'Stale issue run/snapshot' end
            if not unresolved(issue) then return nil,'Issue is already closed' end
            if user then issue.status,issue.risk_reason='accepted_risk',args.response
            else issue.status,issue.response=args.status,args.response end
            ledger.revision=ledger.revision+1
            return save(store,ledger)
        end
    end
    return nil,'Unknown issue ID'
end
function M.respond(args) return respond(args,false) end
-- Manual command adapter only. This function is not part of the model tool.
function M.accept_risk(args) return respond(args,true) end
local function visible(text) return text:gsub('[%z\1-\31\127]',' ') end
function M.display(issue_id)
    local ledger,err=M.read(); if not ledger then return err end
    local lines={'Issues · revision '..ledger.revision}
    for _, issue in ipairs(ledger.issues) do
        if not issue_id or issue.id == issue_id then
            table.insert(lines,string.format('[%s] %s · %s · %s',issue.status,issue.id,
                issue.finding.severity,visible(issue.finding.description)))
            if issue_id then
                table.insert(lines,'  Run: '..visible(issue.run_id)..' · snapshot: '..visible(issue.snapshot))
                table.insert(lines,'  Evidence: '..visible(issue.finding.evidence))
                if issue.finding.file then table.insert(lines,'  File: '..visible(issue.finding.file)..
                    (issue.finding.start_line and (':'..issue.finding.start_line..'-'..issue.finding.end_line) or '')) end
                if issue.response ~= '' then table.insert(lines,'  Response: '..visible(issue.response)) end
                if issue.recheck ~= '' then table.insert(lines,'  Recheck: '..visible(issue.recheck)) end
                if issue.risk_reason ~= '' then table.insert(lines,'  Accepted risk: '..visible(issue.risk_reason)) end
            end
        end
    end
    if #lines == 1 then return issue_id and 'Unknown issue ID' or 'No review issues.' end
    return table.concat(lines,'\n')
end
return M
