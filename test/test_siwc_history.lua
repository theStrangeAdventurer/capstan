package.path = './?.lua;' .. package.path
local binding = require('agent.siwc_history')
local format = require('agent.openai_siwc_responses').history_format
local native = {format=format, items={{type='reasoning', encrypted_content='fixture-opaque'}}, tool_names={}}
local stamped = assert(binding.stamp(native, 'fixture-account-a'))
assert(stamped ~= native and native.account_id == nil)
assert(stamped.account_id == 'fixture-account-a' and stamped.format == format)
assert(stamped.items ~= native.items and stamped.items[1] ~= native.items[1])
assert(stamped.items[1].encrypted_content == 'fixture-opaque')
assert(stamped.tool_names ~= native.tool_names)
local messages = {{role='user', content='question'}, {role='assistant', responses=stamped}}
assert(binding.validate(messages, 'fixture-account-a') == true)
local function rejected(messages, account)
    local ok, err = binding.validate(messages, account)
    assert(ok == nil and type(err) == 'string')
    assert(not err:find('fixture-', 1, true) and not err:find('opaque', 1, true))
end
rejected(messages, 'fixture-account-b')
rejected(messages, nil)
rejected(messages, '')
rejected(messages, 123)
rejected({{role='assistant', responses=native}}, 'fixture-account-a')
for _, value in ipairs({'', 123, false}) do
    local malformed = assert(binding.stamp(native, 'fixture-account-a'))
    malformed.account_id = value
    rejected({{role='assistant', responses=malformed}}, 'fixture-account-a')
end
local other = assert(binding.stamp(native, 'fixture-account-b'))
rejected({messages[1], messages[2], {role='assistant', responses=other}}, 'fixture-account-a')
for _, value in ipairs({false, 'fixture-invalid', {}, {format='openai_siwc.responses.v2'}, {format='private-plugin'}}) do
    rejected({{role='assistant', responses=value}}, 'fixture-account-a')
    local out, err = binding.stamp(value, 'fixture-account-a')
    assert(out == nil and type(err) == 'string')
end
rejected({{role='user', responses=stamped}}, 'fixture-account-a')
assert(binding.stamp(native, nil) == nil)
assert(binding.stamp(native, '') == nil)
assert(binding.stamp(native, 123) == nil)
assert(binding.validate({}, nil) == true)
assert(binding.validate({{role='assistant', content='legacy text'},
    {role='tool', tool_call_id='fixture-call', content='result'}}, nil) == true)
print('SIWC history account binding tests passed')
