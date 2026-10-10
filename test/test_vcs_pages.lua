package.path = './?.lua;' .. package.path
local json = require('vendor.rxi.json')
local calls, content, session = 0, '', 'one'
capstan = {config = {tool_output = {max_bytes = 1024, max_lines = 1}}}
agent = {session_id = function() return session end}
package.loaded['agent.workspace'] = {configured_workspace_root=function() return '/repo' end}
package.loaded['agent.vcs'] = {
    run=function(operation) calls=calls+1; return {adapter='git',operation=operation,output=content} end,
    resolve_path=function(path) return '/repo/'..path end,
}
local plugin = dofile('plugins/vcs.lua')
local function read(args)
    return plugin.handler({tool_args=args, replace=function(_, value) return value end,
        error=function(_, err) return nil,err end})
end
content = string.rep('строка "quoted" \\ newline\n', 4000)
local expected, pages, offset, snapshot = content, {}, 0
repeat
    local encoded = assert(read({operation='diff',path='file',offset=offset,snapshot=snapshot}))
    assert(#encoded <= 1024)
    assert(not select(2, require('agent.tool_output').bound(encoded)))
    local page = json.decode(encoded)
    assert(page.offset == offset and page.total_bytes == #expected)
    pages[#pages+1] = page.output
    snapshot = page.snapshot
    content = 'changed after first page'
    if page.complete then assert(page.next_offset == nil); break end
    assert(page.next_offset > offset)
    offset=page.next_offset
until false
assert(table.concat(pages) == expected and #pages > 100 and calls == 1)
assert(not read({operation='status',path='file',snapshot=snapshot,offset=0}))
assert(not read({operation='diff',path='other',snapshot=snapshot,offset=0}))
assert(not read({operation='diff',path='file',snapshot=snapshot,offset=1})) -- UTF-8 boundary
assert(not read({operation='diff',offset=-1}))
assert(not read({operation='diff',offset=10}))
session='other'
assert(not read({operation='diff',path='file',snapshot=snapshot,offset=0}))
session='one'
for _=1,8 do assert(read({operation='diff'})) end
assert(not read({operation='diff',path='file',snapshot=snapshot,offset=0}))
content=''
assert(json.decode(assert(read({operation='diff'}))).complete)
print('VCS snapshot paging tests passed')

-- The adapter must reject truncation at the native capture layer as well.
package.loaded['agent.vcs']=nil
package.loaded['agent.state']={vcs_for_workspace=function() end}
capstan.config.vcs={default='custom',adapters={custom={commands={diff={'fixture','diff'}}}}}
tools={exec=function() return {exit=0,stdout='partial',stderr='',truncated=true} end}
local result, err=require('agent.vcs').run('diff')
assert(not result and err:find('capture limit',1,true))
