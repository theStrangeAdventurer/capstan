local M = {}

local profiles = {
    fast = {
        name = "fast",
        label = "Fast",
        reasoning_effort = "low",
        completion_review = false,
        prompt = [[
## Active Profile: Fast
Move quickly and keep tool use lean. Use the smallest amount of exploration that
can produce a correct answer, avoid broad plans unless required, and finish with
a terse result.
]],
    },
    implement = {
        name = "implement",
        label = "Implement",
        reasoning_effort = "medium",
        completion_review = true,
        prompt = [[
## Active Profile: Implement
Make focused, correct changes. Read relevant files before editing, keep scope
tight, prefer targeted edits to existing files, and validate with the project's
appropriate command before finishing.

For a scoped change, establish expected behavior from the relevant test,
specification, or caller and form one concrete hypothesis before editing. Run a
focused existing check early when it can cheaply confirm that hypothesis. After
editing, validate the directly affected behavior; inspect a reported failure
before making another speculative change. Do not expand exploration or
validation once the required behavior is evidenced.
]],
    },
    plan = {
        name = "plan",
        label = "Plan",
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
    },
}

local order = {"fast", "implement", "plan"}

local function normalize(value)
    if type(value) ~= "string" then return nil end
    local name = value:lower():gsub("^%s+", ""):gsub("%s+$", "")
    if name == "" then return nil end
    if profiles[name] then return name end
    return nil
end

function M.normalize(value)
    return normalize(value)
end

function M.get(value)
    local name = normalize(value)
    return name and profiles[name] or nil
end

function M.names()
    local out = {}
    for _, name in ipairs(order) do table.insert(out, name) end
    return out
end

local function tool_name(tool)
    if type(tool) ~= "table" then return nil end
    local fn = tool["function"]
    if type(fn) == "table" then return fn.name end
    return nil
end

function M.filter_tools(tools, profile)
    if not profile or not profile.allowed_tools then return tools end
    local filtered = {}
    for _, tool in ipairs(tools or {}) do
        local name = tool_name(tool)
        if name and profile.allowed_tools[name] then
            table.insert(filtered, tool)
        end
    end
    return filtered
end

return M
