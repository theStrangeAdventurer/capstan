-- Standalone: vendor/lua-5.5.0/src/lua test/test_process_tools.lua
local calls = {}
_G.capstan = {config = {}, log = function() end}
_G.tools = {
    shell = function(command, timeout, background)
        calls[#calls + 1] = {command, timeout, background}
        if background then return {started = true, id = "proc-1", pid = 42, status = "running"} end
        return {exit = 0, stdout = "ok", stderr = "", timed_out = false}
    end,
    processes = function(action, id) return {action = action, id = id, status = "running"} end,
    process_stop = function(id) return {id = id, status = "stopping"} end,
}
local shell = dofile("plugins/shell.lua")
local function manual(text)
    return shell.handler({input = "/shell " .. text, command = "/shell",
        replace = function(_, text) return text end})
end
for _, text in ipairs({"--background --timeout 7 printf 'a b'", "--timeout 7 --background printf 'a b'"}) do
    local _, out, ok = manual(text)
    assert(ok and out:find("[started]", 1, true) and not out:find("[exit 0]", 1, true))
    assert(calls[#calls][1] == "printf 'a b'" and calls[#calls][2] == 7 and calls[#calls][3])
end
manual("--background sleep 10")
assert(calls[#calls][2] == 0)
manual("echo ok")
assert(calls[#calls][2] == 60 and not calls[#calls][3])
shell.handler({tool_args = {command = "sleep 10", background = true, timeout = 999}})
assert(calls[#calls][2] == 300)
shell.handler({tool_args = {command = "sleep 10", background = true}})
assert(calls[#calls][2] == 0)
local processes = dofile("plugins/processes.lua")
local stop = dofile("plugins/process_stop.lua")
assert(processes.tool.permission == false and stop.tool.permission == "process_stop")
assert(stop.tool.permission_target({id = "proc-1"}) == "proc-1")
local _, out, ok = processes.handler({tool_args = {action = "output", id = "proc-1"}})
assert(ok and out:find('"output"') and out:find('"proc%-1"'))
local _, _, invalid = processes.handler({tool_args = {action = "stop", id = "proc-1"}})
assert(invalid == false)
local _, stopped, stop_ok = stop.handler({tool_args = {id = "proc-1"}})
assert(stop_ok and stopped:find("stopping") and not stopped:find("exited"))
local plan = dofile("profiles/plan.lua")
assert(plan.allowed_tools.processes and not plan.allowed_tools.process_stop)

local runtime = require("agent.tools")
local function upvalue(fn, wanted)
    for i = 1, 100 do
        local name, value = debug.getupvalue(fn, i)
        if not name then break end
        if name == wanted then return value end
    end
    error("Missing upvalue " .. wanted)
end
local mark = upvalue(runtime.handle_tool_calls, "mark_validation")
local run = {state = {workspace_mutated = true}, session_id = "session-a", update_status = false}
mark(run, "shell", {command = "make test", background = true}, true)
assert(not run.state.successful_validation)
mark(run, "shell", {command = "make test"}, true)
assert(run.state.successful_validation)
local execute = upvalue(runtime.handle_tool_calls, "execute_tool")
local owner = "previous"
tools.process_scope = function(next_owner)
    local old = owner
    owner = next_owner
    return old
end
plugins = {test = {id = "scope_test", tool = {name = "scope_test"}, handler = function()
    assert(owner == "session-a")
    error("expected test error")
end}}
local _, success = execute("scope_test", {}, run)
assert(success == false and owner == "previous")
plugins.test.handler = function() assert(owner == "session-a"); return "ok", "ok", true end
local result, success2 = execute("scope_test", {session_id = "forged"}, run)
assert(success2 and result == "ok" and owner == "previous")
print("process tools: passed")
