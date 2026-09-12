local json = require("vendor.rxi.json")
local redact = require("agent.redact")
local plugin = {
    id = "process_stop", name = "Stop process", history = false,
    description = "Request managed process termination", async = false,
    tool = {
        name = "process_stop",
        description = "Request termination of a managed process by opaque ID (not PID). Only your session's processes or explicitly permitted shared runtime MCP processes can be stopped. Stopping is not confirmation of exit.",
        permission = "process_stop",
        permission_target = function(args) return args.id end,
        parameters = {type = "object", properties = {
            id = {type = "string", description = "Managed process ID, never a PID"},
        }, required = {"id"}},
    },
}
function plugin.handler(ctx)
    local id = ctx.tool_args and ctx.tool_args.id or (ctx.args or {})[1]
    if type(id) ~= "string" or id == "" then
        return "Managed process ID required", "Managed process ID required", false
    end
    local ok, result = pcall(tools.process_stop, id)
    local out = redact.text(ok and ("Stop requested: " .. json.encode(result)) or tostring(result))
    return out, out, ok
end
return plugin
