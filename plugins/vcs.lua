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
    description = "Inspect workspace version-control status or diff. When complete=false, repeat the same operation/path with snapshot and next_offset as offset until complete=true before concluding a review.",
    parameters = {
        type = "object",
        properties = {
            operation = {
                type = "string",
                enum = { "status", "diff", "changes" },
                description = "Read-only VCS operation.",
            },
            path = { type = "string", description = "Optional workspace-relative path for changes." },
            snapshot = {type = "string", description = "Snapshot returned by an earlier page. Keep operation/path unchanged."},
            offset = {type = "integer", minimum = 0, description = "Byte offset: use the preceding page's next_offset."},
        },
        required = { "operation" },
    },
    permission = "file_read",
    permission_target = function(args)
        local root = workspace.configured_workspace_root()
        if type(args.path) ~= "string" or args.path == "" then return root end
        local resolved = vcs.resolve_path(args.path)
        if resolved then return resolved end
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

-- Bounded process-local snapshots: pagination must never re-run a changing diff.
local snapshots, order, serial = {}, {}, 0
local generation = tostring(os.time()) .. tostring({}):gsub("[^%w]", "")
local function scope()
    return workspace.configured_workspace_root() .. "\n" ..
        tostring(agent and agent.session_id and agent.session_id() or '')
end
local function page(args, authorized)
    local snapshot
    local offset = args.offset or 0
    if type(offset) ~= 'number' or offset < 0 or offset ~= math.floor(offset) or offset == math.huge then
        return nil, 'offset must be a nonnegative integer'
    end
    if args.snapshot then
        snapshot = snapshots[args.snapshot]
        if not snapshot or snapshot.scope ~= scope() or snapshot.path ~= args.path or
            snapshot.operation ~= args.operation then
            return nil, 'Snapshot unavailable or scope changed; restart this VCS read'
        end
        if args.path then
            local resolved = vcs.resolve_path(args.path)
            if not resolved or resolved ~= snapshot.resolved or (authorized and authorized ~= resolved) then
                return nil, 'VCS path changed; restart this read'
            end
        end
    else
        if offset ~= 0 then return nil, 'Continuation requires snapshot and offset' end
        local result, err = vcs.run(args.operation, args.path, authorized)
        if not result then return nil, err end
        local output = require('agent.utf8').sanitize(require('agent.redact').text(result.output))
        serial = serial + 1
        local id = generation .. "-" .. tostring(serial)
        snapshot = {scope=scope(), path=args.path, operation=args.operation,
            resolved=args.path and vcs.resolve_path(args.path), adapter=result.adapter, output=output, id=id}
        snapshots[id] = snapshot
        order[#order+1] = id
        if #order > 8 then snapshots[table.remove(order, 1)] = nil end
    end
    local output = snapshot.output
    if offset > #output or (offset < #output and output:byte(offset+1) >= 128 and output:byte(offset+1) < 192) then
        return nil, 'offset is outside the snapshot or splits a UTF-8 character'
    end
    local budget = require('agent.tool_output').limits()
    local size = math.min(#output-offset, budget)
    while true do
        while size > 0 and offset+size < #output and
            output:byte(offset+size+1) >= 128 and output:byte(offset+size+1) < 192 do size=size-1 end
        local done = offset+size == #output
        local encoded = json.encode({adapter=snapshot.adapter, operation=snapshot.operation,
            output=output:sub(offset+1,offset+size), snapshot=snapshot.id, offset=offset,
            next_offset=not done and offset+size or nil, total_bytes=#output, complete=done})
        local _, truncated = require('agent.tool_output').bound(encoded)
        if not truncated and (done or size > 0) then return encoded end
        if size == 0 then return nil, 'Tool output budget is too small for VCS page metadata' end
        size=math.floor(size/2)
    end
end

function plugin.handler(ctx)
    if ctx.tool_args then
        local result, err = page(ctx.tool_args, ctx.permission and ctx.permission.target)
        if not result then return ctx:error("VCS error: " .. tostring(err)) end
        return ctx:replace(result)
    end
    local name = ctx.args and ctx.args[1]
    if not name then return ctx:replace("Select a VCS adapter") end
    local ok, err = vcs.select(name)
    if not ok then return ctx:replace("Cannot select VCS: " .. tostring(err)) end
    return ctx:replace("VCS adapter: " .. name, "")
end

return plugin
