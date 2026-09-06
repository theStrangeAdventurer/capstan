-- Dispatcher + real built-in handlers; virtual filesystem keeps this test isolated.
local tools = require('agent.tools')
local workspace = require('agent.workspace')
local json = require('vendor.rxi.json')
plugins.file = dofile('plugins/file.lua')
plugins.file_write = dofile('plugins/file_write.lua')
plugins.file_edit = dofile('plugins/file_edit.lua')
capstan.workdir = '/repo/project'
capstan.workspace_root = '/repo/project'
capstan.wiki_path = '/wiki'
local files = {['/outside/config.lua'] = 'old', ['/repo/project/note.md'] = 'note'}
local aliases = {['/repo/project/link'] = '/outside/config.lua'}
local opens, writes, prompts = 0, 0, {}
local decision, denied = 'allow', nil
local scope = {}
capstan.realpath = function(path)
    if aliases[path] then return aliases[path] end
    if files[path] or path == '/' or path == '/repo/project' or path == '/outside' then return path end
end
io.open = function(path, mode)
    opens = opens + 1
    if mode == 'rb' and not files[path] then return nil, 'missing' end
    return {
        read = function() return files[path] end,
        write = function(_, ...)
            writes = writes + 1
            local text = table.concat({...})
            files[path] = mode == 'ab' and (files[path] or '') .. text or text
            return true
        end,
        close = function() return true end,
    }
end
os.execute = function(command)
    -- Simulate a dangling link; other absent paths and mkdir succeed.
    if command:find('/outside/dangling', 1, true) then return nil end
    return true
end
permit.check = function(_, target)
    if target == denied then return 'deny' end
    return 'ask'
end
local available = tools.collect({disable_subagents = true})
local function run(name, args)
    opens, writes, prompts = 0, 0, {}
    local result
    tools.handle_tool_calls({}, available, {{id = 'external', name = name,
        arguments = json.encode(args)}}, '', function(msgs) result = msgs[#msgs].content end,
        {permission_scope = scope, callbacks = {
            on_permission_request = function(tool, target)
                prompts[#prompts + 1] = {tool, target}
                return decision
            end,
        }})
    return result
end
local path = '/outside/config.lua'
decision = 'deny'
run('file_write', {path = path, content = 'bad'})
assert(opens == 0 and writes == 0 and files[path] == 'old')
decision = 'allow_session'
run('file_edit', {path = path, old_text = 'old', new_text = 'new'})
assert(files[path] == 'new' and writes == 1 and #prompts == 1)
assert(prompts[1][1] == 'file_write' and prompts[1][2] == path)
run('file_write', {path = path, content = '+', mode = 'append'})
assert(files[path] == 'new+' and #prompts == 0)
run('file_read', {path = path})
assert(#prompts == 1 and prompts[1][1] == 'file_read')
run('file_write', {path = '/outside/other.lua', content = 'other'})
assert(#prompts == 1 and files['/outside/other.lua'] == 'other')
run('file_write', {path = '/outside/new/deep/config.lua', content = 'created'})
assert(files['/outside/new/deep/config.lua'] == 'created')
local result = run('file_write', {path = '/outside/dangling', content = 'bad'})
assert(writes == 0 and result:find('dangling symlink', 1, true))
result = run('file_edit', {path = '/repo/project/link', old_text = 'new+', new_text = 'bad'})
assert(writes == 0 and result:find('escapes workspace', 1, true))
-- No permission context means no external access, even for existing files.
assert(not workspace.model_path_allowed(path, 'write'))
assert(not workspace.model_path_allowed('/outside/unapproved/new.lua', 'write'))
-- Mixed reads authorize all paths first; a grant does not authorize an internal escape.
result = run('file_read', {paths = {'note.md', path, '/outside/other.lua'}})
assert(opens == 3 and result:find('new+', 1, true))
result = run('file_read', {paths = {path, 'link'}})
assert(opens == 1 and result:find('escapes workspace', 1, true))
denied = path
scope.yolo = true
run('file_write', {path = path, content = 'bad'})
assert(opens == 0 and writes == 0 and files[path] == 'new+')
run('file_read', {paths = {'note.md', path}})
assert(opens == 0)
denied = nil
scope.workdir_only = true
scope.full_control = true
run('file_write', {path = path, content = 'bad'})
assert(opens == 0 and writes == 0 and #prompts == 0)
run('file_read', {paths = {'note.md', path}})
assert(opens == 0 and #prompts == 0)
assert(capstan.workdir == '/repo/project' and capstan.workspace_root == '/repo/project')
