local issues = require('agent.issues')
local json = require('vendor.rxi.json')
local plugin = {
    id='issues', name='Review Issues', command='/issues', history=false,
    description='Show review findings independently of task plans',
}
plugin.tool = {
    name='issues', permission=false,
    description='Read session review issues or respond to one. Read first for revision, run_id and snapshot. Executors may mark pending_verification or dispute with evidence, never resolve, delete, publish a reviewer verdict or accept risk. Changes only session metadata.',
    parameters={type='object',properties={
        operation={type='string',enum={'read','respond'}},
        revision={type='integer'},id={type='string'},run_id={type='string'},snapshot={type='string'},
        status={type='string',enum={'pending_verification','disputed'}},
        response={type='string',description='Fix/check evidence or reason for disputing'},
    },required={'operation'}},
}
function plugin.handler(ctx)
    local args=ctx.tool_args
    if args then
        local ledger,err
        if args.operation == 'read' then ledger,err=issues.read()
        elseif args.operation == 'respond' then ledger,err=issues.respond(args)
        else err='Unknown issues operation: use read or respond' end
        if not ledger then return err,err,false end
        return ctx:replace(issues.display(),json.encode(ledger))
    end
    args=ctx.args or {}
    if #args == 0 then return ctx:replace(issues.display(),'') end
    if args[1] == 'history' and #args <= 2 then
        local archive,err=issues.history()
        if not archive then return err,'',false end
        if args[2] then
            for _, run in ipairs(archive.runs) do
                if run.id == args[2] then
                    local found={run=run,issues=json.array({})}
                    for _, issue in ipairs(archive.issues) do
                        if issue.run_id == run.id then table.insert(found.issues,issue) end
                    end
                    return ctx:replace(json.encode(found),'')
                end
            end
            for _, issue in ipairs(archive.issues) do
                if issue.id == args[2] then return ctx:replace(json.encode(issue),'') end
            end
            return ctx:replace('Unknown archived run or issue ID','')
        end
        return ctx:replace(json.encode(archive),'')
    end
    if #args == 1 and args[1] ~= 'accept' then return ctx:replace(issues.display(args[1]),'') end
    if args[1] == 'accept' and #args >= 3 then
        local ledger,err=issues.read()
        if not ledger then return err,'',false end
        for _, issue in ipairs(ledger.issues) do
            if issue.id == args[2] then
                ledger,err=issues.accept_risk({revision=ledger.revision,id=issue.id,run_id=issue.run_id,
                    snapshot=issue.snapshot,response=table.concat(args,' ',3)})
                if not ledger then return err,'',false end
                return ctx:replace(issues.display(issue.id),'')
            end
        end
        return 'Unknown issue ID','',false
    end
    return ctx:replace('Usage: /issues [id] | /issues history [run-id|issue-id] | /issues accept <id> <reason>','')
end
return plugin
