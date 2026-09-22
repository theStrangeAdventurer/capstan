-- Completion-review snapshots. No live filesystem fallback is permitted.
-- Native adapter contract:
-- tools.review_snapshot_list(root) -> array {path=relative, kind="file"|"directory"|
--   "symlink", ignored=boolean}, or nil,error. Recursively enumerates all entries,
--   applies project ignore rules, never follows links (including root ancestors),
--   never traverses ignored directories, and fails on incomplete enumeration.
-- tools.review_snapshot_read(root, relative, max_bytes, authorize?) -> exact bytes or nil,error.
--   Root-confined descriptor-relative, no-follow opens at EVERY component; rejects
--   non-regular files, truncation and files exceeding max_bytes. No redaction or
--   text transformation: drift comparisons need exact original bytes.
-- authorize receives absolute paths, must be a non-prompting permission query.
-- This is cooperative, not an OS lock. External writers can race either scan.
local workspace = require("agent.workspace")
local M = {}
local barriers, states = {}, setmetatable({}, {__mode = "k"})
local MAX_FILE, MAX_TOTAL, MAX_FILES, MAX_OUTPUT = 4*1024*1024, 32*1024*1024, 20000, 65536

local function root_path(root)
    if type(root) ~= "string" or root:sub(1, 1) ~= "/" or root:find("%z") then
        return nil, "absolute workspace root required"
    end
    local normalized = workspace.normalize_path(root)
    local real = workspace.realpath(normalized)
    if not real or real ~= normalized then return nil, "workspace root cannot be safely resolved" end
    return real
end
local function sensitive(path)
    for part in path:gmatch("[^/]+") do
        local p = part:lower()
        if workspace.is_sensitive_path(part) or p:match("^%.env") or
            p:match("^%.zshenv") or p == ".ssh" or p == ".aws" or
            p == ".gnupg" or p == ".netrc" or p == ".npmrc" or
            p == ".bashrc" or p == ".bash_profile" or p == ".profile" or
            p == ".zshrc" or p:match("%.pem$") or p:match("%.key$") then return true end
    end
    return false
end
local function relative(path)
    if type(path) ~= "string" or path == "" or path:sub(1,1) == "/" or
        path:find("%z") or path:find("\\",1,true) then return false end
    for part in path:gmatch("[^/]+") do
        if part == "." or part == ".." then return false end
    end
    return not path:find("//",1,true) and path:sub(-1) ~= "/"
end
local function full(root, path) return (root == "/" and "" or root) .. "/" .. path end
local function immutable(t)
    return setmetatable({}, {__index=t, __newindex=function() error("immutable snapshot",2) end,
        __pairs=function() return next,t,nil end, __len=function() return #t end, __metatable=false})
end
-- Stable non-cryptographic identifier; correctness ALWAYS compares full bytes.
local function digest(text)
    local h = 5381
    for i=1,#text do h = (h * 33 + text:byte(i)) % 4294967296 end
    return string.format("%08x:%d",h,#text)
end
local function scan(root, authorize)
    local native = _G.tools
    if not native or type(native.review_snapshot_list) ~= "function" or
        type(native.review_snapshot_read) ~= "function" then
        return nil,"safe native snapshot primitives unavailable"
    end
    local ok, entries, err = pcall(native.review_snapshot_list,root)
    if not ok or type(entries) ~= "table" then return nil,err or "snapshot enumeration failed" end
    if #entries > MAX_FILES then return nil,"snapshot entry limit exceeded" end
    local excluded, seen = {}, {}
    for _,entry in ipairs(entries) do
        if type(entry) ~= "table" or not relative(entry.path) or seen[entry.path] or
            type(entry.ignored) ~= "boolean" or
            (entry.kind ~= "file" and entry.kind ~= "directory" and entry.kind ~= "symlink") then
            return nil,"invalid native snapshot entry"
        end
        seen[entry.path] = true
        if entry.ignored or entry.kind == "symlink" or sensitive(entry.path) then
            excluded[entry.path] = true
        end
    end
    local files, denied, total = {}, {}, 0
    for _,entry in ipairs(entries) do
        local path, skip = entry.path, false
        for prefix in pairs(excluded) do
            if path == prefix or path:sub(1,#prefix+1) == prefix .. "/" then skip=true; break end
        end
        if not skip and entry.kind == "file" then
            local allowed, value = pcall(authorize,full(root,path))
            if not allowed then return nil,"snapshot authorization query failed" end
            if value ~= true then denied[path]=true
            else
                local read_ok, content = pcall(native.review_snapshot_read,root,path,MAX_FILE,authorize)
                if not read_ok or type(content) ~= "string" then return nil,"safe snapshot read failed: " .. path end
                total=total+#content
                if #content > MAX_FILE or total > MAX_TOTAL then return nil,"snapshot byte limit exceeded" end
                files[path]=content
            end
        end
    end
    return {files=files, denied=denied}
end
local function same(a,b)
    for k,v in pairs(a) do if b[k] ~= v then return false end end
    for k,v in pairs(b) do if a[k] ~= v then return false end end
    return true
end
local function stable(a,b) return same(a.files,b.files) and same(a.denied,b.denied) end

function M.capture(root,authorize)
    if type(authorize) ~= "function" then return nil,"non-prompting authorization query required" end
    local resolved,err=root_path(root)
    if not resolved then return nil,err end
    if sensitive(resolved) then return nil,"sensitive workspace root excluded" end
    local first; first,err=scan(resolved,authorize)
    if not first then return nil,err end
    local second; second,err=scan(resolved,authorize)
    if not second then return nil,err end
    if not stable(first,second) then return nil,"workspace changed during snapshot capture" end
    local paths, hashes, version_parts = {}, {}, {}
    for path in pairs(first.files) do paths[#paths+1]=path end
    table.sort(paths)
    for _,path in ipairs(paths) do
        hashes[path]=digest(first.files[path])
        version_parts[#version_parts+1]=#path .. ":" .. path .. ":" .. hashes[path]
    end
    local state={root=resolved, files=first.files, denied=first.denied, authorize=authorize}
    local methods={root=resolved,version=digest(table.concat(version_parts,"\n")),
        files=immutable(first.files),digests=immutable(hashes),paths=immutable(paths),
        complete=next(first.denied)==nil}
    local snapshot
    function methods.check()
        if state.released then return false,"snapshot released" end
        local current,problem=scan(resolved,authorize)
        if not current then return false,problem end
        if not stable(state,current) then return false,"workspace drift: snapshot is stale" end
        return true
    end
    function methods.diff(_,baseline)
        if state.released then return nil,"snapshot released" end
        local base=states[baseline]
        if not base or base.released or base.root ~= state.root then
            return nil,"baseline unavailable; change attribution unknown"
        end
        if not same(base.denied,state.denied) then return nil,"permission scope changed; attribution unknown" end
        local changes={}
        for path,content in pairs(state.files) do
            if base.files[path] ~= content then changes[#changes+1]={path=path,
                status=base.files[path] == nil and "added" or "modified",
                before=base.files[path],after=content} end
        end
        for path,content in pairs(base.files) do
            if state.files[path] == nil then changes[#changes+1]={path=path,status="deleted",before=content} end
        end
        table.sort(changes,function(a,b) return a.path < b.path end)
        -- Renames intentionally remain deletion+addition at original paths.
        return changes
    end
    local required, coverage = {}, {}
    local function covered(path, first, last)
        local ranges=coverage[path] or {}
        ranges[#ranges+1]={first,last}
        table.sort(ranges,function(a,b) return a[1]<b[1] end)
        local merged={}
        for _,range in ipairs(ranges) do
            local tail=merged[#merged]
            if tail and range[1]<=tail[2] then tail[2]=math.max(tail[2],range[2])
            else merged[#merged+1]=range end
        end
        coverage[path]=merged
        local progress=merged[1][1]==0 and merged[1][2] or 0
        required[path]=progress<#state.files[path] and progress or nil
    end
    function methods.access_complete()
        if next(required)==nil then return true end
        local pending={}
        for path in pairs(required) do pending[#pending+1]=path end
        table.sort(pending)
        local details={}
        for i=1,math.min(#pending,8) do
            local path=pending[i]
            details[#details+1]=string.format('%q offset=%d',path:sub(1,160),required[path])
        end
        if #pending>8 then details[#details+1]=string.format('and %d more files',#pending-8) end
        return false,'unread pages remain: '..table.concat(details,'; ')
    end
    function methods.read(_,args)
        if state.released then return nil,"snapshot released","access" end
        if type(args) ~= "table" then return nil,'read arguments required; use {"path":"file"}',"invalid_arguments" end
        local requested=workspace.file_read_paths(args)
        if #requested==0 then return nil,'snapshot path required; use {"path":"file"}',"invalid_arguments" end
        local paged=args.offset~=nil or args.limit~=nil
        local offset=args.offset==nil and 0 or args.offset
        local limit=args.limit==nil and 48000 or args.limit
        if paged and (#requested~=1 or type(offset)~='number' or offset<0 or offset%1~=0 or
            type(limit)~='number' or limit<4 or limit>48000 or limit%1~=0) then
            return nil,'paging requires one path, integer offset >= 0 and limit 4..48000; use {"path":"file","offset":0,"limit":48000} or {"paths":["a","b"]} without offset/limit','invalid_arguments'
        end
        local result,size,normalized={},0,{}
        for _,path in ipairs(requested) do
            if path:sub(1,1)=="/" then
                local prefix=(resolved=="/" and "" or resolved).."/"
                if path:sub(1,#prefix) ~= prefix then return nil,"path outside snapshot","access" end
                path=path:sub(#prefix+1)
            end
            if not relative(path) or sensitive(path) or state.files[path]==nil then return nil,"path unavailable in snapshot","access" end
            normalized[#normalized+1]=path
            local content=state.files[path]
            if paged then
                local function continuation(pos)
                    local byte=content:byte(pos)
                    return byte and byte>=128 and byte<192
                end
                if offset>#content or continuation(offset+1) then
                    return nil,'invalid byte offset; retry offset=0 or the previous next_offset with one path and limit=48000','invalid_arguments'
                end
                local last=math.min(#content,offset+limit)
                while last>offset and continuation(last+1) do last=last-1 end
                covered(path,offset,last)
                return string.format('%s [offset=%d next_offset=%d total_bytes=%d eof=%s]\n',
                    path,offset,last,#content,tostring(last==#content))..content:sub(offset+1,last)
            end
            local text=path .. "\n" .. content
            size=size+#text+(#result>0 and 1 or 0)
            result[#result+1]=text
        end
        if size>MAX_OUTPUT then
            for _,path in ipairs(normalized) do covered(path,0,0) end
            return nil,'snapshot read exceeds output limit (65536 bytes); retry one path with offset=0, limit=48000','size'
        end
        for _,path in ipairs(normalized) do covered(path,0,#state.files[path]) end
        return table.concat(result,"\n")
    end
    function methods.release()
        state.released=true
        for path in pairs(state.files) do state.files[path]=nil end
        for path in pairs(hashes) do hashes[path]=nil end
        for i=#paths,1,-1 do paths[i]=nil end
        state.authorize=nil
    end
    snapshot=immutable(methods)
    states[snapshot]=state
    return snapshot
end

-- Conservative: nonterminal managed jobs may write any workspace. MCP records
-- represent persistent transports, not jobs: tools/call is synchronous and new
-- MCP tool dispatch is blocked by the write barrier in agent.tools. Waiting for
-- transport exit would deadlock every review with a connected stdio server.
-- Autonomous server-side work remains outside this cooperative barrier.
-- No process is killed. Caller retries within its own review budget.
function M.writers_active()
    if not _G.tools or type(_G.tools.processes) ~= "function" then
        return true,"managed process enumeration unavailable"
    end
    local ok,records=pcall(_G.tools.processes,"list")
    if not ok or type(records) ~= "table" then return true,"managed process enumeration failed" end
    for _,record in ipairs(records) do
        if type(record) ~= "table" then return true,"invalid managed process record" end
        if record.kind ~= "mcp" and record.status ~= "completed" and
            record.status ~= "failed" and record.status ~= "cancelled" and
            record.status ~= "exited" and record.status ~= "timed_out" then
            return true,"active potential workspace writer: "..tostring(record.id or "unknown")
        end
    end
    return false
end
function M.blocked(root)
    if next(barriers)==nil then return false end
    local resolved=root_path(root)
    if not resolved then return true,"workspace root unresolved" end
    for owner,base in pairs(barriers) do
        if workspace.path_is_within(resolved,base) or workspace.path_is_within(base,resolved) then
            return true,"completion review write barrier",owner
        end
    end
    return false
end
function M.acquire(owner,root)
    if owner==nil then return nil,"barrier owner required" end
    local resolved,err=root_path(root)
    if not resolved then return nil,err end
    if barriers[owner] and barriers[owner] ~= resolved then return nil,"owner already holds another workspace" end
    local blocked,reason,holder=M.blocked(resolved)
    if blocked and holder ~= owner then return nil,reason end
    -- Retained while waiting so NEW managed writes cannot prolong the wait.
    barriers[owner]=resolved
    local active,problem=M.writers_active()
    if active then return nil,problem end
    return true
end
function M.release(owner) barriers[owner]=nil end
return M
