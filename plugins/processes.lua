local json = require("vendor.rxi.json")
local redact = require("agent.redact")
local tool_output = require("agent.tool_output")
local plugin = {
    id = "processes", name = "Processes", history = false,
    description = "Inspect managed processes", async = false,
    tool = {
        name = "processes",
        description = "Read managed process snapshots or bounded output. Model calls are session-scoped. Use opaque IDs, never PIDs. Running is not success; unavailable output (e.g. MCP protocol) is not an empty successful result.",
        permission = false,
        parameters = {type = "object", properties = {
            action = {type = "string", enum = {"list", "get", "output"}},
            id = {type = "string", description = "Managed process ID for get/output"},
        }},
    },
}
function plugin.handler(ctx)
    local args = ctx.tool_args or {action = (ctx.args or {})[1], id = (ctx.args or {})[2]}
    local action = args.action or "list"
    if action ~= "list" and action ~= "get" and action ~= "output" then
        return "Unknown processes action", "Unknown processes action", false
    end
    if action ~= "list" and (type(args.id) ~= "string" or args.id == "") then
        return "Managed process ID required", "Managed process ID required", false
    end
    local ok, result = pcall(tools.processes, action, args.id)
    local out = redact.text(ok and json.encode(result) or tostring(result))
    return tool_output.bound(out), out, ok
end
return plugin
