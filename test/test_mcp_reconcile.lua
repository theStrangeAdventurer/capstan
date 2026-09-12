-- Run with the vendored Lua interpreter from the repository root.
package.path = './?.lua;' .. package.path
package.loaded['agent.logging'] = {runtime_log = function() end, compact = tostring}
package.loaded['agent.images'] = {}
local json = require('vendor.rxi.json')
local spawned, procs = 0, {}
_G.capstan = {now_ms = function() return 0 end, config = {mcp = {
  enabled = true, servers = {{name = 'global', command = '/bin/cat'}}
}}}
_G.mcp = {
  spawn = function(_, _, _, owner)
    spawned = spawned + 1
    procs[spawned] = {owner = owner, alive = true, replies = {}}
    return spawned
  end,
  alive = function(h) return procs[h].alive end,
  kill = function(h) procs[h].alive = false end,
  send = function(h, text)
    local req = json.decode(text)
    if req.id then
      local result = req.method == 'tools/list' and
        {tools = {{name = 'echo', inputSchema = {type = 'object'}}}} or {}
      table.insert(procs[h].replies, json.encode({id = req.id, result = result}))
    end
    return true
  end,
  recv_nowait = function(h) return table.remove(procs[h].replies, 1), 'again' end,
}
local client = require('agent.mcp')
client.init()
for _ = 1, 4 do client.tick() end
assert(client.is_initialized())
assert(procs[1].owner == 'runtime')
assert(#client.collect_tools() == 1)
assert(client.attach_scope('actual-acp-session', {
  {name = 'scoped', command = '/bin/cat', args = json.decode('[]'), env = json.decode('[]')}
}))
for _ = 1, 4 do client.tick_scope('actual-acp-session') end
assert(client.is_scope_initialized('actual-acp-session'))
assert(procs[2].owner == 'actual-acp-session')
assert(#client.collect_tools('actual-acp-session') == 2)
assert(#client.collect_tools('other-session') == 1)
procs[1].alive = false
local text, ok = client.call('mcp__global__echo', {})
assert(not ok and text:find('reconnect'))
procs[2].alive = false
assert(client.tick_scope('actual-acp-session'))
assert(#client.collect_tools('actual-acp-session') == 0)
assert(client.is_mcp_tool('mcp__scoped__echo', 'actual-acp-session'))
text, ok = client.call('mcp__scoped__echo', {}, 'actual-acp-session')
assert(not ok and text:find('reconnect'))
for _ = 1, 5 do client.tick(); client.tick_scope('actual-acp-session') end
assert(spawned == 2)
-- A native spawn error is a failed server, not a crashed initialization pass.
_G.mcp.spawn = function() error('exec failed') end
assert(client.attach_scope('failed-acp-session', {
  {name = 'broken', command = '/missing', args = json.decode('[]'), env = json.decode('[]')}
}))
client.tick_scope('failed-acp-session')
assert(client.is_scope_initialized('failed-acp-session'))
assert(#client.collect_tools('failed-acp-session') == 0)
print('MCP reconciliation passed')
