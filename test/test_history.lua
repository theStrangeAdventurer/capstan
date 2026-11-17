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
local binding=require('agent.siwc_history')
local native=assert(binding.stamp({format='openai_siwc.responses.v1',
    items={{type='reasoning',encrypted_content='fixture-opaque'}},tool_names={}}, 'fixture-account-a'))
local native_encoded=history.encode({{role='assistant',content='native final',responses=native}}, {}, {}, 'native final')
local native_restored=history.expand({{role='assistant',model_history_json=native_encoded}})
assert(#native_restored==1)
assert(native_restored[1].responses.account_id=='fixture-account-a')
assert(native_restored[1].responses.items[1].encrypted_content=='fixture-opaque')
assert(binding.validate(native_restored, 'fixture-account-a')==true)
assert(binding.validate(native_restored, 'fixture-account-b')==nil)
local legacy_native=history.expand({{role='assistant',model_history_json=json.encode({
    {role='assistant',responses={format='openai_siwc.responses.v1',items={},tool_names={}}}})}})
assert(binding.validate(legacy_native, 'fixture-account-a')==nil)
print('Structured history adapter tests passed')
