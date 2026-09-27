package.path = "./?.lua;" .. package.path
local review = require("agent.review_snapshot")
_G.capstan = {realpath=function(path) if path == "/link" then return "/elsewhere" end return path end}
local disk, extra, reads, processes, denied, on_list
local function setup()
    disk={ ["existing.txt"]="pre-existing user edits", ["context.txt"]="dependency" }
    extra={}
    reads={}
    processes={}
    denied={}
    on_list=nil
    _G.tools={
        review_snapshot_list=function(root)
            assert(root=="/work")
            if on_list then on_list() end
            local entries={}
            for path in pairs(disk) do entries[#entries+1]={path=path,kind="file",ignored=false} end
            for _,entry in ipairs(extra) do entries[#entries+1]=entry end
            return entries
        end,
        review_snapshot_read=function(root,path,max)
            assert(root=="/work" and max>0)
            reads[path]=(reads[path] or 0)+1
            assert(not path:find(".env",1,true) and not path:find(".zshenv",1,true))
            return disk[path]
        end,
        processes=function(action) assert(action=="list"); return processes end,
    }
end
local function authorize(path) return not denied[path] end
local function capture() return assert(review.capture("/work",authorize)) end
setup()
local baseline=capture()
assert(baseline.complete and baseline:check())
assert(not pcall(function() baseline.files["existing.txt"]="tamper" end))
assert(not pcall(function() baseline.version="tamper" end))
assert(#baseline:diff(capture())==0)
disk["existing.txt"]="task edit"
disk["new.txt"]="new"
disk["context.txt"]=nil
local current=capture()
local changes=current:diff(baseline)
assert(#changes==3)
assert(changes[1].path=="context.txt" and changes[1].status=="deleted")
assert(changes[2].before=="pre-existing user edits" and changes[2].status=="modified")
assert(changes[3].status=="added")
assert(current.version~=baseline.version)
assert(baseline:read({path="existing.txt"}):find("pre-existing user edits",1,true))
assert(not baseline:check())
assert(current:check())
assert(not current:diff(nil))
assert(not current:read({path="../escape"}))
assert(not current:read({path="/outside/existing.txt"}))
assert(current:read({path="/work/new.txt"})=="new.txt\nnew")
disk["added-after.txt"]="drift"
assert(not current:check())
current:release()
assert(not current:check() and not current:read({path="new.txt"}))
assert(next(current.files)==nil)

setup()
-- No forbidden bytes are opened, including descendants of excluded entries.
disk[".env"]="never read"
disk[".zshenv"]="never read"
disk["dir/.env.production"]="never read"
disk[".ssh/config"]="never read"
disk["skip/data.txt"]="never read"
disk["link/data.txt"]="never read"
disk["private.txt"]="permission denied"
extra={{path="skip",kind="directory",ignored=true},{path="link",kind="symlink",ignored=false}}
denied["/work/private.txt"]=true
local filtered=capture()
assert(not filtered.complete)
for _,path in ipairs({".env",".zshenv","dir/.env.production",".ssh/config","skip/data.txt","link/data.txt","private.txt"}) do
    assert(not reads[path] and not filtered.files[path],path)
end
assert(filtered:check())
denied["/work/existing.txt"]=true
assert(not filtered:check())

setup()
local dependency=capture()
disk["context.txt"]="dependency changed"
assert(not dependency:check())
setup()
local count=0
on_list=function() count=count+1; disk["existing.txt"]=tostring(count) end
assert(not review.capture("/work",authorize))
setup()
extra={{path="../escape",kind="file",ignored=false}}
assert(not review.capture("/work",authorize))
assert(next(reads)==nil)
setup()
assert(not review.capture("/link",authorize))
assert(not review.capture("/work",nil))
assert(not review.capture("/work",function() error("denied") end))
tools.review_snapshot_read=nil
local missing,err=review.capture("/work",authorize)
assert(not missing and err:find("primitives unavailable",1,true))
local function output_error(snapshot, args)
    local text, problem=snapshot:read(args)
    assert(text==nil and problem:find("output limit (65536 bytes)",1,true),problem)
end
for _,size in ipairs({65535,65536,65537,100000}) do
    setup()
    disk["big.txt"]=string.rep("a",size-#"big.txt\n")
    local snap=capture()
    if size<=65536 then
        assert(snap:read({path="big.txt"})=="big.txt\n"..disk["big.txt"])
    else output_error(snap,{path="big.txt"}) end
end
-- Headers and the separator count, even when both individual reads fit.
for _,size in ipairs({65535,65536,65537}) do
    setup()
    disk={a=string.rep("a",size-5),b=""}
    local snap=capture()
    assert(snap:read({path="a"})=="a\n"..disk.a)
    if size<=65536 then
        assert(snap:read({paths={"a","b"}})=="a\n"..disk.a.."\nb\n")
    else output_error(snap,{paths={"a","b"}}) end
end
-- An exactly full first entry must not hide a later requested file.
setup()
disk={a=string.rep("a",65534),tail="must not disappear"}
local full=capture()
output_error(full,{paths={"a","tail"}})
assert(not full:read({paths={"a","missing"}}))
-- Count UTF-8 bytes, not characters; never return a partial code point.
setup()
local unicode_path="данные.txt"
local prefix=unicode_path.."\n"
disk={[unicode_path]=string.rep("я",math.floor((65536-#prefix)/2))}
local unicode=capture()
assert(unicode:read({path=unicode_path})==prefix..disk[unicode_path])
disk[unicode_path]=disk[unicode_path].."🙂"
output_error(capture(),{path=unicode_path})

setup()
local enumerated=capture()
tools.review_snapshot_list=function() return nil,"enumeration failed" end
assert(not review.capture("/work",authorize))
local valid,problem=enumerated:check()
assert(not valid and problem=="enumeration failed")
setup()
tools.review_snapshot_read=function() return nil,"read failure" end
assert(not review.capture("/work",authorize))

setup()
processes={{status="running",kind="shell"}}
assert(not review.acquire("owner","/work"))
assert(review.blocked("/work/subdir")) -- lock retained while draining
assert(not review.acquire("other","/work"))
processes={{status="exited"},{status="completed"},{status="timed_out"}}
assert(review.acquire("owner","/work"))
assert(not review.blocked("/work-other"))
review.release("other")
assert(review.blocked("/work"))
review.release("owner")
assert(not review.blocked("/work"))
-- A persistent MCP transport must not prevent review (runtime or scoped).
for _,owner in ipairs({"runtime","session-1"}) do
    processes={{id="mcp-1",kind="mcp",owner=owner,status="running"}}
    assert(not review.writers_active())
    assert(review.acquire("mcp-review","/work"))
    assert(review.blocked("/work"))
    review.release("mcp-review")
end
-- Exempt only transports, never unrelated jobs beside them.
for _,kind in ipairs({"shell","argv","subagent","unknown"}) do
    processes={{kind="mcp",status="running"},{id="job-1",kind=kind,status="running"}}
    local acquired,why=review.acquire("drain","/work")
    assert(not acquired and why:find("job-1",1,true))
    assert(review.blocked("/work"))
    processes[2].status="completed"
    assert(review.acquire("drain","/work"))
    review.release("drain")
end
processes={false}
assert(review.writers_active())
tools.processes=nil
assert(not review.acquire("unknown","/work"))
review.release("unknown")
print("review_snapshot: all tests passed (mock native filesystem, no disk reads)")

-- Oversized reads can recover by paging exact immutable UTF-8 bytes.
setup()
disk['large.txt']=string.rep('я🙂',20000)
local paged=capture()
local original=disk['large.txt']
assert(not paged:read({path='large.txt'}))
assert(not paged:access_complete())
disk['large.txt']='new live content'
local offset,parts=0,{}
repeat
    local page=assert(paged:read({path='large.txt',offset=offset,limit=47999}))
    local header,body=page:match('^(.-)\n(.*)$')
    assert(utf8.len(body))
    parts[#parts+1]=body
    offset=tonumber(header:match('next_offset=(%d+)'))
until offset==#original
assert(table.concat(parts)==original and paged:access_complete())
assert(not paged:check())
assert(not paged:read({path='large.txt',offset=1,limit=10}))
assert(not paged:read({paths={'large.txt','existing.txt'},offset=0}))
local gap=capture()
assert(gap:read({path='existing.txt',offset=5,limit=10}))
assert(not gap:access_complete(), 'skipping earlier bytes must not complete coverage')
print('review_snapshot: recoverable UTF-8 paging passed')

-- Explicit categories, empty files and monotonic coverage across retries.
setup()
disk={a=string.rep('x',70000),b=string.rep('y',70000),empty=''}
local retry=capture()
for _,args in ipairs({{}, {paths={'a','b'},limit=48000}, {path='a',limit=3},
    {path='a',limit=48001}, {path='a',offset=-1}, {path='a',offset=1.5},
    {path='a',offset=false}, {path='a',limit=false}, {path='a',offset=70001}}) do
    local value,why,category=retry:read(args)
    assert(not value and why and category=='invalid_arguments')
end
local value,_,category=retry:read({path='../outside'})
assert(not value and category=='access')
value,_,category=retry:read({paths={'a','b','empty'}})
assert(not value and category=='size' and not retry:access_complete())
assert(retry:read({path='empty',offset=0,limit=4}))
-- Out-of-order ranges become complete once the gap is filled.
for _,path in ipairs({'a','b'}) do
    assert(retry:read({path=path,offset=48000,limit=48000}))
    assert(not retry:access_complete())
    assert(retry:read({path=path,offset=0,limit=48000}))
end
assert(retry:access_complete())
assert(retry:read({path='a',offset=0,limit=4}))
assert(retry:access_complete())
assert(not retry:read({paths={'a','b'}}))
assert(retry:access_complete(), 'failed reread must retain already delivered coverage')
retry:release()
value,_,category=retry:read({path='a'})
assert(not value and category=='access')
setup()
disk={}
for i=1,20 do disk[string.format('file%02d',i)]=string.rep('x',5000) end
local diagnostic=capture()
local paths={}; for path in pairs(disk) do paths[#paths+1]=path end
assert(not diagnostic:read({paths=paths}))
local complete,detail=diagnostic:access_complete()
assert(not complete and #detail<2000 and detail:find('offset=0',1,true))
assert(detail:find('and 12 more files',1,true) and not detail:find('xxxxx',1,true))
print('review_snapshot: categories, monotonic coverage and bounded diagnostics passed')

-- Full dedup identity is byte-exact, sorted and independent of the short version.
setup()
disk={a='alpha', b='beta'}
local f1=capture()
local fp1=f1:full_fingerprint()
assert(type(fp1)=='string' and fp1:find('1:a',1,true) and fp1:find('5:alpha',1,true))
assert(fp1:find('4:beta',1,true) and fp1:find('a',1,true) < fp1:find('b',1,true))
local f2=capture()
assert(f2:full_fingerprint()==fp1, 'same bytes must produce the same identity')
disk.a='altered'
local f3=capture()
assert(f3:full_fingerprint()~=fp1, 'changed bytes must change the identity')
f3:release()
assert(f3:full_fingerprint()==nil, 'released snapshots expose no identity')
f1:release(); f2:release()
print('review_snapshot: full dedup identity passed')


-- Review includes dirty changes that predate the request, against HEAD.
setup()
tools.review_snapshot_head=function(root,auth)
    assert(root=='/work' and auth('/work/existing.txt'))
    return {['existing.txt']='committed', ['deleted.txt']='removed'}
end
local head=assert(review.capture('/work',authorize,true))
local dirty=capture()
local delta=assert(dirty:diff(head))
assert(#delta==3)
assert(delta[1].path=='context.txt' and delta[1].status=='added')
assert(delta[2].path=='deleted.txt' and delta[2].before=='removed')
assert(delta[3].before=='committed' and delta[3].after=='pre-existing user edits')
head:release(); dirty:release()
print('review_snapshot: pre-existing worktree changes versus HEAD passed')

-- The baseline comes from the configured VCS adapter, never a hardcoded Git
-- assumption. A non-Git adapter fails closed without touching the native blob
-- reader; the built-in Git adapter still uses it.
setup()
local head_calls = 0
tools.review_snapshot_head = function(root, auth)
    assert(root == '/work' and auth('/work/existing.txt'))
    head_calls = head_calls + 1
    return {['existing.txt'] = 'committed'}
end
_G.capstan.config = { vcs = { adapters = {
    hg = { label = 'Mercurial', commands = { status = { 'hg', 'status' } } },
} } }
_G.capstan.state = { vcs_by_workspace = { ['/work'] = 'hg' } }
local ok, err = review.capture('/work', authorize, true)
assert(not ok and err:find('hg', 1, true) and err:find('baseline', 1, true), tostring(err))
assert(head_calls == 0, 'non-Git adapter must not invoke the native Git blob reader')
_G.capstan.state.vcs_by_workspace['/work'] = nil
local git_head = assert(review.capture('/work', authorize, true))
assert(git_head.files['existing.txt'] == 'committed' and head_calls == 2,
    'capture scans twice for drift')
git_head:release()
_G.capstan.config = nil
_G.capstan.state = nil
print('review_snapshot: baseline routes through the VCS adapter passed')
