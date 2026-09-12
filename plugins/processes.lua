local json = require("vendor.rxi.json")
local redact = require("agent.redact")
local tool_output = require("agent.tool_output")
local plugin = {
    id = "processes", name = "Processes", history = false,
    description = "Inspect managed processes", async = false,
    tool = {
        name = "processes",
        description = "Read managed process/subagent snapshots or bounded output; wait explicitly before final conclusions that depend on completion. Wait timeout is not task failure. Model calls are session-scoped. Use opaque IDs, never PIDs. Running is not success; unavailable output (e.g. MCP protocol) is not an empty successful result.",
        permission = false,
        parameters = {type = "object", properties = {
            action = {type = "string", enum = {"list", "get", "output", "wait"}},
            id = {type = "string", description = "Managed process/group/task ID for get/output/wait"},
            timeout = {type = "number", description = "Wait timeout in seconds (default 30, maximum 300). Zero returns immediately."},
        }},
    },
}
local function now_ms()
    if _G.capstan and type(capstan.now_ms) == "function" then return capstan.now_ms() end
    return os.time() * 1000
end
local function wait(id, timeout)
    timeout = tonumber(timeout) or 30
    if timeout ~= timeout or timeout < 0 then error("Wait timeout must be non-negative") end
    timeout = math.min(timeout, 300)
    local deadline = now_ms() + timeout * 1000
    while true do
        -- Native get enforces scope on every iteration, including after callbacks.
        local snapshot = tools.processes("get", id)
        if type(snapshot) ~= "table" then error("Managed process snapshot unavailable") end
        local status = snapshot.status
        local terminal = status == "completed" or status == "failed" or status == "cancelled" or
            status == "exited" or (snapshot.running == false and status ~= "queued" and status ~= "running")
        if terminal then snapshot.timed_out = false; return snapshot end
        if now_ms() >= deadline then snapshot.timed_out = true; return snapshot end
        if not _G.http or type(http.poll) ~= "function" then error("processes wait: http.poll unavailable") end
        if type(_G.agent_background_wait_poll) == "function" then
            _G.agent_background_wait_poll()
        else
            http.poll()
            require("agent.subagents").poll()
        end
        if type(http.wait_frame) == "function" then http.wait_frame() end
    end
end
function plugin.handler(ctx)
    local args = ctx.tool_args or {action = (ctx.args or {})[1], id = (ctx.args or {})[2]}
    local action = args.action or "list"
    if action ~= "list" and action ~= "get" and action ~= "output" and action ~= "wait" then
        return "Unknown processes action", "Unknown processes action", false
    end
    if action ~= "list" and (type(args.id) ~= "string" or args.id == "") then
        return "Managed process ID required", "Managed process ID required", false
    end
    local ok, result
    if action == "wait" then ok, result = pcall(wait, args.id, args.timeout)
    else ok, result = pcall(tools.processes, action, args.id) end
    local out = redact.text(ok and json.encode(result) or tostring(result))
    return tool_output.bound(out), out, ok
end
return plugin
