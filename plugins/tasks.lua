local tasks = require('agent.tasks')
local json = require('vendor.rxi.json')
local plugin = {
    id = 'tasks', name = 'Task Plan', command = '/tasks', history = false,
    description = 'Show the persistent task plan for this session',
}
plugin.tool = {
    name = 'tasks', permission = false,
    description = 'Read or update the persistent session task plan. In Plan, create and refine a plan with this tool. In Implement, create a plan only when the user asks; if a plan exists, consult it and track execution. Read first to obtain revision. Updates upsert complete task records by stable ID, preserving omitted tasks. Cancel abandoned tasks instead of deleting them. Completed, blocked and cancelled tasks require a result/reason. This changes session metadata only, never project files.',
    parameters = {
        type = 'object',
        properties = {
            operation = {type='string', enum={'read', 'update'}},
            revision = {type='integer', description='Current revision, required for update'},
            tasks = {type='array', items={type='object', properties={
                id={type='string', description='Stable ASCII ID using letters, digits, underscore or hyphen'},
                title={type='string'},
                status={type='string', enum={'pending','in_progress','completed','blocked','cancelled'}},
                criteria={type='string', description='Acceptance criteria'},
                result={type='string', description='What was done and checked, or reason blocked/cancelled'},
            }, required={'id','title','status'}}},
        }, required={'operation'},
    },
}
function plugin.handler(ctx)
    if not ctx.tool_args then return ctx:replace(tasks.display(), '') end
    local args = ctx.tool_args
    local plan, err
    if args.operation == 'read' then plan, err = tasks.read()
    elseif args.operation == 'update' then plan, err = tasks.update(args)
    else err = 'Unknown tasks operation: use read or update.' end
    if not plan then return err, err, false end
    return ctx:replace(tasks.display(), json.encode(plan))
end
return plugin
