-- Offline regression: cancellation persists the runtime's final checkpoint.
package.path = './?.lua;./?/init.lua;' .. package.path
local json = require('vendor.rxi.json')
local function noop() end
package.loaded['agent.images'] = {}
package.loaded['agent.mcp'] = {ensure_initialized=noop, is_initialized=function() return true end,
    is_scope_initialized=function() return true end, attach_scope=function() return true end,
    close_scope=noop}
package.loaded['agent.profiles'] = {get=function() return {name='implement'} end,
    names=function() return {'implement'} end}
package.loaded['agent.tasks'] = {use_store=noop}
package.loaded['agent.issues'] = {use_store=noop}
local sent, runs = {}, {}
acp = {send=function(s) sent[#sent+1]=json.decode(s) end,
    set_directory=function() return true end, commands=function() return {} end,
    cancel=function() error('runtime cancellation expected') end}
capstan = {now_ms=function() return 0 end, agent={get_profile=function() return 'implement' end,
    run=function(opts, callbacks)
        local run={opts=opts, callbacks=callbacks}
        runs[#runs+1]=run
        callbacks.on_history('[{"role":"assistant","content":"tool round"}]')
        callbacks.on_text('partial answer')
        return true, nil, function()
            callbacks.on_history('[{"role":"assistant","content":"tool round"},{"role":"assistant","content":"partial answer"}]')
            callbacks.on_done({ok=false,cancelled=true,text=''})
        end
    end}}
plugins = {}
require('agent.acp')
local function request(id, method, params)
    capstan_acp_handle(json.encode({jsonrpc='2.0',id=id,method=method,params=params}))
end
request(1,'initialize',{protocolVersion=1})
request(2,'session/new',{cwd='/workspace',mcpServers={}})
local session
for _, message in ipairs(sent) do
    if message.id==2 then session=message.result.sessionId end
end
assert(session)
request(3,'session/prompt',{sessionId=session,prompt={{type='text',text='first'}}})
request(nil,'session/cancel',{sessionId=session})
assert(not capstan_acp_active())
local count=0
for _, message in ipairs(sent) do
    if message.id==3 then
        count=count+1
        assert(message.result.stopReason=='cancelled')
    end
end
assert(count==1, 'cancel must respond exactly once')
request(4,'session/prompt',{sessionId=session,prompt={{type='text',text='continue'}}})
local messages=runs[2].opts.messages
assert(#messages==3)
local history=json.decode(messages[2].model_history_json)
assert(#history==2 and history[2].content=='partial answer')
-- Closing an active session uses the same checkpoint-before-persistence path.
request(5,'session/close',{sessionId=session})
assert(not capstan_acp_active())
count=0
for _, message in ipairs(sent) do
    if message.id==4 then count=count+1; assert(message.result.stopReason=='cancelled') end
end
assert(count==1)
print('ACP cancellation history tests passed')
