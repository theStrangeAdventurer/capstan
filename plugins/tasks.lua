local tasks = require('agent.tasks')
local json = require('vendor.rxi.json')
local plugin = {
    id = 'tasks', name = 'Task Plan', command = '/tasks', history = false,
    description = 'Show the persistent task plan for this session',
}
plugin.tool = {
    name = 'tasks', permission = false,
    description = 'Read, update or clear the persistent session task plan. Clear a completed/cancelled plan before starting unrelated work or when the user requests cleanup, never automatically after each response. Unfinished tasks must first be completed or explicitly cancelled with a reason. In Plan, create and refine a plan with this tool. In Implement, create a plan only when the user asks; if a plan exists, consult it and track execution. Read first to obtain revision. Updates upsert complete task records by stable ID, preserving omitted tasks. Cancel abandoned tasks instead of deleting them. Completed, blocked and cancelled tasks require a result/reason. This changes session metadata only, never project files.',
    parameters = {
        type = 'object',
        properties = {
            operation = {type='string', enum={'read', 'update', 'clear'}},
            revision = {type='integer', description='Current revision, required for update or clear'},
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
    if not ctx.tool_args then
        local args = ctx.args or {}
        if #args == 0 then return ctx:replace(tasks.display(), '') end
        if #args ~= 1 or args[1] ~= 'clear' then
            return ctx:replace('Usage: /tasks [clear]', '')
        end
        local plan, err = tasks.read()
        if plan then plan, err = tasks.clear({revision=plan.revision}) end
        if not plan then return err, '', false end
        return ctx:replace('Task plan cleared.', '')
    end
    local args = ctx.tool_args
    local plan, err
    if args.operation == 'read' then plan, err = tasks.read()
    elseif args.operation == 'update' then plan, err = tasks.update(args)
    elseif args.operation == 'clear' then plan, err = tasks.clear(args)
    else err = 'Unknown tasks operation: use read, update or clear.' end
    if not plan then return err, err, false end
    return ctx:replace(tasks.display(), json.encode(plan))
end
return plugin
