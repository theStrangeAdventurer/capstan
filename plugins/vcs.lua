local json = require("vendor.rxi.json")
local vcs = require("agent.vcs")
local workspace = require("agent.workspace")

local plugin = {
    id = "vcs",
    name = "VCS",
    description = "Inspect changes or select a version-control adapter",
    command = "/vcs",
    history = false,
}

plugin.tool = {
    name = "vcs",
    description = "Inspect workspace version-control status or diff using the configured read-only VCS adapter.",
    parameters = {
        type = "object",
        properties = {
            operation = {
                type = "string",
                enum = { "status", "diff", "changes" },
                description = "Read-only VCS operation.",
            },
            path = { type = "string", description = "Optional workspace-relative path for changes." },
        },
        required = { "operation" },
    },
    permission = "file_read",
    permission_target = function(args)
        local root = workspace.configured_workspace_root()
        if type(args.path) ~= "string" or args.path == "" then return root end
        local resolved = vcs.resolve_path(args.path)
        if resolved then
            args._resolved_vcs_path = resolved
            return resolved
        end
        local requested = workspace.normalize_path(args.path, root)
        return workspace.realpath(requested) or requested
    end,
}

plugin.autocomplete = {
    title = "VCS adapter",
    limit = 10,
    multi = false,
    fetch = function()
        local current = vcs.current()
        local items = {}
        for name, adapter in pairs(vcs.adapters()) do
            table.insert(items, {
                text = string.format("%s %s", name == current and "*" or " ", adapter.label),
                value = name,
            })
        end
        table.sort(items, function(a, b) return a.text < b.text end)
        return items
    end,
}

function plugin.handler(ctx)
    if ctx.tool_args then
        local result, err = vcs.run(ctx.tool_args.operation, ctx.tool_args.path,
            ctx.tool_args._resolved_vcs_path)
        if not result then return ctx:error("VCS error: " .. tostring(err)) end
        return ctx:replace(json.encode(result))
    end
    local name = ctx.args and ctx.args[1]
    if not name then return ctx:replace("Select a VCS adapter") end
    local ok, err = vcs.select(name)
    if not ok then return ctx:replace("Cannot select VCS: " .. tostring(err)) end
    return ctx:replace("VCS adapter: " .. name, "")
end

return plugin
