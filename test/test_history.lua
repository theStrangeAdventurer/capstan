package.path = './?.lua;' .. package.path
local history=require('agent.history')
local json=require('vendor.rxi.json')
local initial={role='user',content='question'}
local transient={role='system',content='runtime state'}
local messages={initial,transient,
    {role='assistant',content='reading',reasoning_content='continuity',
     tool_calls={{id='call',type='function',['function']={name='file_read',arguments='{"path":"a"}'}}}},
    {role='tool',tool_call_id='call',content='result'},
    {role='user',content={{type='image_url',image_url={url='data:image/png;base64,YWJj'}}}},
}
local encoded=history.encode(messages,{[initial]=true},{[transient]=true},'final')
local restored=history.expand({initial,{role='assistant',content='UI-only text',model_history_json=encoded}})
assert(#restored==5 and restored[1].content=='question')
assert(restored[2].reasoning_content=='continuity')
assert(restored[2].tool_calls[1]['function'].arguments=='{"path":"a"}')
assert(restored[3].role=='tool' and restored[3].tool_call_id=='call')
assert(restored[4].content[1].image_url.url=='data:image/png;base64,YWJj')
assert(restored[5].content=='final')
for _,value in ipairs({'{','[]','[1]','[{"role":"system","content":"bad"}]'}) do
    assert(not pcall(history.expand,{{role='assistant',model_history_json=value}}))
end
assert(history.expand({{role='assistant',content='legacy'}})[1].content=='legacy')
assert(history.encode({initial},{[initial]=true},{},'')==nil)
assert(#json.decode(encoded)==4)
print('Structured history adapter tests passed')
