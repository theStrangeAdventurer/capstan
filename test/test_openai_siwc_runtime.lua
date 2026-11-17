-- Offline: vendor/lua-5.5.0/src/lua test/test_openai_siwc_runtime.lua
package.path = './?.lua;./?/init.lua;' .. package.path
local function noop() end
local logs={}
local function log(_, value) logs[#logs+1]=tostring(value) end
local env = {}
os.getenv = function(name) return env[name] end -- Never read real credentials.
package.loaded['agent.state'] = {provider = noop, profile_models = function() return {} end,
    model_for = noop, model_reasoning_effort = noop}
capstan = {version = 'test-version', config = {}, workdir = '.', workspace_root = '.'}
package.loaded['agent.logging'] = {runtime_log = log, compact = tostring, safe_error = tostring,
    debug = noop, raw_logging_enabled = function() return false end}
package.loaded['agent.hooks'] = {has = function() return false end,
    run = function(_, ctx) return ctx end, install_config = noop, install_existing_plugins = noop}
package.loaded['agent.mcp'] = {}
package.loaded['agent.ui'] = {append = noop}
package.loaded['agent.tasks'] = {refresh = noop}
package.loaded['agent.tools'] = {collect = function() return {} end, names = function() return '' end,
    handle_tool_calls = function(messages, available, _, _, continue) continue(messages, available) end}
local json = require('vendor.rxi.json')
local available, generation = true, 1
local selected, accesses, child_snapshot = 'fixture-account', {}, nil
local secrets = {['fixture-account']='fixture-secret', ['fixture-other']='other-fixture-secret'}
package.loaded['agent.openai_siwc_auth'] = {access=function(id)
    accesses[#accesses+1] = id or selected
    id = id or selected
    if not available or not secrets[id] then return nil, 'missing' end
    return {access=secrets[id], account_id=id, generation=generation}
end}
capstan.config = {provider='openai_siwc', providers={openai_codex={model='private',endpoint='https://private.invalid'}}}
agent = {set_info=noop, set_usage=noop, set_profile_info=noop}
popup = {error=noop}
local requests, gets, history, done = {}, 0
http = {cancel=noop, get=function(url, headers, options)
    assert(options.https_only == true and options.follow_redirects == false)
    assert(url=='https://api.openai.com/v1/models' and headers.Authorization=='Bearer fixture-secret')
    gets=gets+1
    return 200, '{"models":[{"slug":"visible-z","display_name":"Zed","visibility":"list","supported_reasoning_levels":["high"],"default_reasoning_level":"high","context_window":99999},{"slug":"visible-a","visibility":"list"},{"slug":"hidden","visibility":"hidden"},{"slug":"missing"},{"slug":"wrong","visibility":true},{"slug":42,"visibility":"list"},{"slug":"","visibility":"list"},false]}'
end, post_stream=function(endpoint, body, headers, callback, timeout, options)
    assert(type(timeout) == 'number' and options.background == false)
    assert(options.https_only == true and options.follow_redirects == false)
    assert(endpoint=='https://api.openai.com/v1/responses')
    assert(headers.Authorization=='Bearer fixture-secret' and not body:find('fixture-secret',1,true))
    requests[#requests+1]={body=json.decode(body),callback=callback}
    return #requests
end}
package.loaded['agent.tools'].handle_tool_calls=function(messages, tools, calls, text, continue, ctx)
    child_snapshot = ctx.provider
    assert(child_snapshot.account_id=='fixture-account')
    selected = 'fixture-other' -- UI switch while the parent is in flight.
    messages[#messages+1]={role='assistant',content=text,responses=ctx.assistant_native_output}
    messages[#messages+1]={role='tool',tool_call_id=calls[1].id,content='fixture result'}
    continue(messages,tools)
end
local runtime=require('agent.runtime')
assert(runtime.providers.openai_codex.model=='private')
local catalog=runtime.list_models('openai_siwc')
assert(#catalog==2)
assert(catalog[1].context_limit==0 and catalog[1].reasoning_efforts==nil)
assert(catalog[1].default_reasoning_level==nil)
local models=require('agent.models')
local siwc=runtime.providers.openai_siwc
assert(not models.validate_siwc_effort(siwc,'visible-z','high'))
siwc.reasoning_efforts={['visible-z']={'low','high'}, ['visible-a']={'medium'}}
siwc.models={{id='visible-z',context_limit=12345}}
local configured=runtime.list_models('openai_siwc')
assert(configured[1].context_limit==12345 and configured[2].context_limit==0)
assert(table.concat(configured[1].reasoning_efforts,',')=='low,high')
assert(table.concat(models.cached_reasoning_efforts(runtime,'openai_siwc','visible-a'),',')=='medium')
assert(models.validate_siwc_effort(siwc,'visible-z','high'))
assert(not models.validate_siwc_effort(siwc,'visible-a','high'))
assert(catalog[1].id=='visible-z' and catalog[1].text=='Zed' and catalog[2].id=='visible-a')
assert(runtime.list_models('openai_siwc') and gets==1)
generation=2
assert(runtime.list_models('openai_siwc') and gets==2)
local function run()
    done=nil
    return runtime.run({messages={{role='user',content='test'}},completion_review=false,
        tools={{type='function',['function']={name='probe',parameters={type='object'}}}}},
        {on_text=noop,on_history=function(v) history=v end,on_done=function(v) done=v end})
end
assert(run())
assert(requests[1], done and done.error or 'no request')
assert(requests[1].body.model=='visible-z' and requests[1].body.store==false)
local function complete(output, text)
    local callback=requests[#requests].callback
    if text then callback('data: '..json.encode({type='response.output_text.delta',delta=text})..'\n\n',false) end
    callback('data: '..json.encode({type='response.completed',response={status='completed',output=output}})..'\n\n',false)
    callback(nil,true)
end
complete({{type='function_call',id='item-1',call_id='call-1',namespace='capstan',name='f_1',arguments='{}',status='completed'}})
assert(#requests==2)
local found=false
for _,item in ipairs(requests[2].body.input) do if item.type=='function_call_output' then found=true end end
assert(found)
complete({{type='message',id='message-1',role='assistant',phase='final_answer',status='completed',content={{type='output_text',text='Done'}}}},'Done')
assert(done and done.ok)
assert(not history:find('fixture-secret',1,true))
local restored=require('agent.history').expand({{role='assistant',model_history_json=history}})
assert(#restored==3 and restored[1].responses.format=='openai_siwc.responses.v1' and restored[3].responses)
assert(restored[3].responses.items[1].phase == 'final_answer')
local replay = require('agent.openai_siwc_responses').build_request({model='visible-z', messages=restored})
assert(replay.input[#replay.input].phase == 'final_answer')
assert(accesses[#accesses]=='fixture-account')
assert(runtime.run({provider='openai_siwc',provider_snapshot=child_snapshot,depth=1,
    messages={{role='user',content='child'}},tools={},completion_review=false},
    {on_text=noop,on_done=function(v) done=v end}))
assert(accesses[#accesses]=='fixture-account')
complete({{type='message',id='child',role='assistant',content={{type='output_text',text='child done'}}}},'child done')
assert(done.ok)
selected='fixture-account'
local before_switch=#requests
runtime.run({provider='openai_codex',messages=restored,tools={},completion_review=false},
    {on_done=function(v) done=v end})
assert(not done.ok and done.error:find('Incompatible native Responses history',1,true))
assert(#requests==before_switch)
assert(run())
local cb=requests[#requests].callback
cb('data: {"type":"response.failed","response":{"error":{"code":"fixture_failure"}}}\n\n',false)
cb(nil,true)
assert(done and not done.ok)
assert(not table.concat(logs):find('fixture-secret',1,true))
local count_before=#requests
runtime.providers.openai_siwc.endpoint='https://fixture.invalid'
run()
assert(done and not done.ok and #requests==count_before)
runtime.providers.openai_siwc.endpoint='https://api.openai.com/v1/responses'
available=false
local count=#requests
assert(not run() and #requests==count and done and not done.ok)
available=true
for _, case in ipairs({
    {'response.failed', {code='insufficient_quota',message='fixture-secret'}, 'quota'},
    {'response.incomplete', {reason='max_output_tokens'}, 'incomplete'},
    {'response.failed', {code='fixture-secret',message='fixture-secret'}, 'protocol'},
}) do
    assert(run())
    local callback=requests[#requests].callback
    callback('data: '..json.encode({type=case[1],response={error=case[2]}})..'\n\n',false)
    callback(nil,true)
    assert(not done.ok and done.error_category==case[3])
    assert(not done.error:find('fixture-secret',1,true))
    if case[3]=='quota' then assert(done.error_code=='insufficient_quota') end
end
-- Late subscription errors retain safe codes and recovery, never run tools or replay immediately.
for _, case in ipairs({
    {'subscription_sharing_usage_limit_exceeded', 'subscription_limit', 'https://chatgpt.com/settings/usage'},
    {'subscription_sharing_usage_unavailable', 'usage_unavailable', 'credentials are preserved'},
}) do
    for _, partial in ipairs({false, true}) do
        assert(run())
        local count = #requests
        local callback = requests[count].callback
        if partial then
            callback('data: {"type":"response.output_text.delta","delta":"provisional"}\n\n', false)
        end
        callback('data: '..json.encode({type='response.failed', response={error={
            code=case[1], message='fixture-secret'}}})..'\n\n', false)
        callback(nil, true)
        assert(done and not done.ok and done.error_code == case[1] and done.error_category == case[2])
        assert(done.error:find(case[3], 1, true) and not done.error:find('fixture-secret', 1, true))
        assert(done.text == '' and done.tool_calls == nil and #requests == count)
        assert(available and secrets['fixture-account'] == 'fixture-secret')
    end
end
capstan.config.agent={max_stream_retries=0}
assert(run())
requests[#requests].callback(nil,true,'connection error fixture-secret')
assert(not done.ok and done.error_category=='transport')
assert(not done.error:find('fixture-secret',1,true))
-- Explicit per-model capability config is the fallback, never catalog guesswork.
local function effort_run(model, effort, default)
    done=nil
    return runtime.run({model=model,reasoning_effort=effort,reasoning_effort_default=default,
        messages={{role='user',content='reasoning'}},tools={},completion_review=false},
        {on_text=noop,on_done=function(v) done=v end})
end
for _, case in ipairs({{'visible-z','high'}, {'visible-a','medium'}}) do
    assert(effort_run(case[1],case[2]))
    assert(requests[#requests].body.reasoning.effort==case[2])
    complete({{type='message',id='effort-result',role='assistant',content={{type='output_text',text='ok'}}}},'ok')
    assert(done.ok)
end
for _, effort in ipairs({'high','invented'}) do
    local before=#requests
    effort_run('visible-a',effort)
    assert(done and not done.ok and done.error:find('Unsupported SIWC reasoning effort',1,true))
    assert(#requests==before)
end
siwc.reasoning_effort='high'
assert(effort_run('visible-a',nil,true))
assert(requests[#requests].body.reasoning==nil)
complete({{type='message',id='effort-result',role='assistant',content={{type='output_text',text='ok'}}}},'ok')
assert(done.ok)
siwc.reasoning_effort=nil
siwc.reasoning={max_tokens=100}
local before=#requests
effort_run('visible-z')
assert(done and not done.ok and done.error:find('Unsupported SIWC reasoning field',1,true))
assert(#requests==before)
siwc.reasoning=nil
assert(restored[1].responses.account_id == 'fixture-account')
local foreign = json.decode(json.encode(restored))
foreign[1].responses.account_id = 'fixture-other'
local before_foreign = #requests
assert(not runtime.run({provider='openai_siwc', messages=foreign, tools={}, completion_review=false},
    {on_done=function(v) done=v end}))
assert(not done.ok and done.error:find('not bound', 1, true) and #requests == before_foreign)
local access_count, request_count=#accesses,#requests
capstan.runtime_options={isolated=true}
package.loaded['agent.runtime']=nil
local isolated=require('agent.runtime')
assert(not isolated.run({provider='openai_siwc',tools={},completion_review=false},
    {on_done=function(v) done=v end}))
assert(done.error:find('isolated',1,true) and #accesses==access_count and #requests==request_count)
assert(not table.concat(logs):find('fixture-secret',1,true))
print('SIWC offline runtime tests passed')
