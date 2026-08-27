return {
    name = "plan",
    label = "Plan",
    order = 30,
    reasoning_effort = "high",
    readonly = true,
    completion_review = false,
    allowed_tools = {
        fetch = true,
        file_read = true,
        logs = true,
        subagents = true,
    },
    prompt = [[
## Active Profile: Plan
You are in read-only planning mode. Explore the codebase and requirements, but
do not modify files or run mutating commands. Do not call file_write,
file_edit, shell, or any other mutating tool. You may use subagents only for
read-only parallel exploration. If changes are needed, describe them as a
concrete numbered plan with files likely to change, risks, and validation
steps. Ask only if a missing requirement materially changes the plan.
]],
}
