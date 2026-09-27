-- Untrusted reviewer output is a protocol, not prose or an instruction source.
local json = require('vendor.rxi.json')
local M = {}
local array_mt = getmetatable(json.array({}))

function M.text(value, limit, required)
    return type(value) == 'string' and #value <= limit and
        not value:find('%z') and (not required or value:find('%S') ~= nil)
end
function M.integer(value, minimum, maximum)
    return type(value) == 'number' and value >= minimum and value <= maximum and value % 1 == 0
end
function M.array(value, limit)
    if type(value) ~= 'table' or getmetatable(value) ~= array_mt or #value > limit then return false end
    local count = 0
    for key in pairs(value) do
        if not M.integer(key, 1, #value) then return false end
        count = count + 1
    end
    return count == #value
end
local function fields(value, allowed)
    if type(value) ~= 'table' or getmetatable(value) ~= nil then return false end
    for key in pairs(value) do if not allowed[key] then return false end end
    return true
end

-- rxi intentionally accepts some non-JSON forms and drops null/duplicate keys.
-- Check framing first so those forms cannot turn an invalid verdict into clean.
function M.decode(raw, limit)
    if type(raw) ~= 'string' or #raw == 0 or #raw > limit then return nil, 'JSON size limit or empty response' end
    local i, n = 1, #raw
    local function space() while i <= n and raw:sub(i,i):match('[ \t\r\n]') do i = i + 1 end end
    local function string_token()
        local start = i
        assert(raw:sub(i,i) == '"'); i = i + 1
        while i <= n do
            local ch = raw:sub(i,i)
            if ch == '"' then
                i = i + 1
                local value = json.decode(raw:sub(start,i-1))
                assert(not value:find('%z'))
                return value
            end
            assert(raw:byte(i) >= 32)
            if ch == '\\' then i = i + 1 end
            i = i + 1
        end
        error('unterminated string')
    end
    local value
    value = function(depth)
        assert(depth <= 16); space()
        local ch = raw:sub(i,i)
        if ch == '"' then string_token(); return end
        if ch == '{' or ch == '[' then
            local object, close, seen = ch == '{', ch == '{' and '}' or ']', {}
            i = i + 1; space()
            if raw:sub(i,i) == close then i = i + 1; return end
            while true do
                if object then
                    local key = string_token(); assert(not seen[key]); seen[key] = true
                    space(); assert(raw:sub(i,i) == ':'); i = i + 1
                end
                value(depth + 1); space()
                ch = raw:sub(i,i); i = i + 1
                if ch == close then return end
                assert(ch == ','); space()
                assert(raw:sub(i,i) ~= close)
            end
        end
        -- Only integers and booleans are needed by this bounded protocol.
        -- Null is forbidden: it must not disappear from arrays or objects.
        local token = raw:sub(i):match('^[^,%]%}%s]+')
        assert(token)
        assert(token == 'true' or token == 'false' or token == '0' or token:match('^%-?[1-9][0-9]*$'))
        i = i + #token
    end
    local ok, decoded = pcall(function()
        value(0); space(); assert(i > n)
        return json.decode(raw)
    end)
    if not ok then return nil, 'Malformed JSON (duplicates, nulls and ambiguous syntax are rejected)' end
    return decoded
end

local severity = {critical=true, high=true, medium=true, low=true}
function M.finding(item)
    if not fields(item, {id=true,severity=true,description=true,evidence=true,file=true,start_line=true,end_line=true}) or
        not severity[item.severity] or not M.text(item.description, 2048, true) or
        not M.text(item.evidence, 4096, true) then return false end
    if item.id ~= nil and not M.text(item.id,64,true) then return false end
    if item.file ~= nil and not M.text(item.file,4096,true) then return false end
    if item.start_line ~= nil or item.end_line ~= nil then
        if not item.file or not M.integer(item.start_line,1,2147483646) or
            not M.integer(item.end_line,item.start_line,2147483646) then return false end
    end
    return true
end
function M.parse(raw)
    local report, err = M.decode(raw, 128 * 1024)
    if not report then return nil, err end
    if not fields(report, {verdict=true,summary=true,findings=true,checks=true}) or
        not ({clean=true,findings=true,inconclusive=true})[report.verdict] or
        not M.text(report.summary,200,true) or not M.array(report.findings,100) or
        not M.array(report.checks,100) then return nil, 'Invalid review report schema' end
    if (report.verdict == 'findings') ~= (#report.findings > 0) then
        return nil, 'Verdict does not match findings'
    end
    local seen = {}
    for _, item in ipairs(report.findings) do
        if not M.finding(item) or (item.id and seen[item.id]) then return nil, 'Invalid or duplicate finding' end
        if item.id then seen[item.id] = true end
    end
    for _, check in ipairs(report.checks) do
        if not fields(check, {id=true,status=true,evidence=true}) or
            not M.text(check.id,64,true) or seen[check.id] or
            not ({resolved=true,open=true,stale=true})[check.status] or not M.text(check.evidence,4096,true) or
            (report.verdict == 'clean' and not ({resolved=true,stale=true})[check.status]) or
            report.verdict == 'inconclusive' then return nil, 'Invalid or duplicate recheck' end
        seen[check.id] = true
    end
    return report
end
return M
