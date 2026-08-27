local state = require("agent.state")
local workspace = require("agent.workspace")

local M = {}

local builtin = {
    git = {
        label = "Git",
        commands = {
            status = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "status", "--short" },
            head = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "rev-parse", "--verify", "HEAD" },
            diff = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "diff", "--no-ext-diff", "--no-textconv", "HEAD", "--" },
            diff_path = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "diff", "--no-ext-diff", "--no-textconv", "HEAD", "--", "{path}" },
            diff_unborn = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "diff", "--no-ext-diff", "--no-textconv", "--cached", "--" },
            diff_path_unborn = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "diff", "--no-ext-diff", "--no-textconv", "--cached", "--", "{path}" },
            diff_worktree = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "diff", "--no-ext-diff", "--no-textconv", "--" },
            diff_path_worktree = { "git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                "diff", "--no-ext-diff", "--no-textconv", "--", "{path}" },
        },
    },
}

local function config()
    local value = _G.capstan and _G.capstan.config and _G.capstan.config.vcs
    return type(value) == "table" and value or {}
end

local function valid_command(command)
    if type(command) ~= "table" or #command == 0 then return false end
    for _, value in ipairs(command) do
        if type(value) ~= "string" or value == "" then return false end
    end
    return true
end

function M.adapters()
    local result = { git = builtin.git }
    local configured = config().adapters
    if type(configured) == "table" then
        for name, adapter in pairs(configured) do
            if type(name) == "string" and name ~= "" and type(adapter) == "table" and
               type(adapter.commands) == "table" then
                local commands = {}
                for operation, command in pairs(adapter.commands) do
                    if valid_command(command) then commands[operation] = command end
                end
                if next(commands) then
                    result[name] = {
                        label = type(adapter.label) == "string" and adapter.label or name,
                        commands = commands,
                    }
                end
            end
        end
    end
    return result
end

local function workspace_root()
    return _G.capstan and _G.capstan.workspace_root or "."
end

function M.current()
    local adapters = M.adapters()
    local saved = state.vcs_for_workspace(workspace_root())
    if saved and adapters[saved] then return saved end
    local configured = config().default
    if type(configured) == "string" and adapters[configured] then return configured end
    return "git"
end

function M.select(name)
    if not M.adapters()[name] then return false, "unknown VCS adapter: " .. tostring(name) end
    return state.set_vcs_for_workspace(workspace_root(), name)
end

local function argv_for(command, path)
    local argv = {}
    local used_path = false
    for _, value in ipairs(command) do
        if value == "{path}" then
            if type(path) ~= "string" or path == "" then
                return nil, "path is required for this operation"
            end
            used_path = true
            table.insert(argv, path)
        else
            table.insert(argv, value)
        end
    end
    if path and not used_path then
        return nil, "path command must contain a {path} argument"
    end
    return argv
end

function M.resolve_path(path)
    if type(path) ~= "string" or path == "" then return nil, "path must be a non-empty string" end
    local root = workspace.real_workspace()
    if not root then return nil, "workspace realpath failed" end
    local requested = workspace.normalize_path(path, workspace.configured_workspace_root())
    local resolved = workspace.realpath(requested)
    if not resolved then return nil, "path does not exist" end
    if not workspace.path_is_within(resolved, root) then
        return nil, "resolved path escapes workspace: " .. resolved
    end
    return resolved
end

function M.run(operation, path, resolved_path)
    local reported_operation = operation
    if path ~= nil then
        if operation ~= "diff" and operation ~= "changes" then
            return nil, "path is supported only for diff and changes"
        end
        local err
        path, err = resolved_path or M.resolve_path(path)
        if not path then return nil, err end
    end
    if operation == "changes" or operation == "diff" then
        operation = path and "diff_path" or "diff"
    end
    local name = M.current()
    local adapter = M.adapters()[name]
    local unborn = false
    if adapter == builtin.git and (operation == "diff" or operation == "diff_path") then
        local head = tools.exec(adapter.commands.head, 30)
        if head.exit ~= 0 then
            unborn = true
            operation = operation .. "_unborn"
        end
    end
    local command = adapter.commands[operation]
    if not command then return nil, "adapter " .. name .. " does not support " .. tostring(operation) end
    local argv, err = argv_for(command, path)
    if not argv then return nil, err end
    local result = tools.exec(argv, 30)
    if result.exit ~= 0 then
        local message = result.stderr ~= "" and result.stderr or result.stdout
        return nil, string.format("%s %s failed (exit %d): %s", name, reported_operation,
            result.exit, message)
    end
    local output = result.stdout
    if unborn then
        local worktree_command = adapter.commands[path and "diff_path_worktree" or "diff_worktree"]
        local worktree_argv, worktree_err = argv_for(worktree_command, path)
        if not worktree_argv then return nil, worktree_err end
        local worktree = tools.exec(worktree_argv, 30)
        if worktree.exit ~= 0 then
            local message = worktree.stderr ~= "" and worktree.stderr or worktree.stdout
            return nil, string.format("%s %s failed (exit %d): %s", name, reported_operation,
                worktree.exit, message)
        end
        output = output .. worktree.stdout
    end
    return { adapter = name, operation = reported_operation, output = output }, nil
end

return M
