local json = require("vendor.rxi.json")
local workspace = require("agent.workspace")

local plugin = {}

plugin.id = "wiki"
plugin.name = "Wiki"
plugin.description = "Show wiki status, ingest Markdown sources, and read configured wiki files"
plugin.command = "/wiki"

local wiki_read_tool = {
    name = "wiki_read",
    description = "Read a Markdown file from the configured Capstan wiki by relative path. Use this when the wiki metadata index suggests the full document is relevant.",
    permission = false,
    parameters = {
        type = "object",
        properties = {
            path = { type = "string", description = "Relative path inside the configured wiki directory, for example contexts/homelab.md" },
        },
        required = { "path" },
    },
}

local wiki_source_read_tool = {
    name = "wiki_source_read",
    description = "Read a Markdown file from an external source root previously indexed by `wiki_ingest` or `/wiki ingest`. Only use source/path pairs visible in the wiki index.",
    permission = false,
    parameters = {
        type = "object",
        properties = {
            source = { type = "string", description = "Indexed source id from the wiki index, for example docs-1a2b3c4d" },
            path = { type = "string", description = "Relative Markdown path inside that indexed source root" },
        },
        required = { "source", "path" },
    },
}

local wiki_ingest_tool = {
    name = "wiki_ingest",
    description = "Index an external Markdown file or directory into the configured Capstan wiki after the user asks to add, index, summarize, or make that source available to the wiki. This creates metadata under wiki/index and optionally copies Markdown into the wiki.",
    permission = "file_read",
    parameters = {
        type = "object",
        properties = {
            path = { type = "string", description = "Markdown file or directory to index" },
            copy = { type = "boolean", description = "When true, copy Markdown into the wiki and write generated frontmatter on the copies. Defaults to false." },
            target_dir = { type = "string", description = "Optional relative directory inside the wiki for copy mode, for example sources/project-notes" },
        },
        required = { "path" },
    },
}

plugin.tools = { wiki_read_tool, wiki_source_read_tool, wiki_ingest_tool }
plugin.tool = wiki_read_tool

local function log(message)
    if _G.log and type(_G.log.info) == "function" then
        _G.log.info("wiki", message)
    end
end

local function notify_info(message)
    if _G.popup and type(_G.popup.info) == "function" then
        _G.popup.info("Wiki ingest", message)
    end
end

local function notify_error(message)
    if _G.popup and type(_G.popup.error) == "function" then
        _G.popup.error("Wiki ingest", message)
    end
end

local function wiki_config()
    return workspace.configured_wiki_path()
end

local function wiki_root()
    return workspace.configured_wiki_root()
end

local function invalid_relative(path)
    return path == nil or path == "" or workspace.is_absolute_path(path) or path:find("%z") ~= nil
end

local function resolve_wiki_path(relative)
    if invalid_relative(relative) then
        return nil, "wiki path must be a non-empty relative path"
    end
    local root = wiki_root()
    if not root then
        return nil, "Wiki is not configured. Set wiki.path in ~/.config/capstan/config.lua."
    end
    local full = workspace.normalize_path(root .. "/" .. relative)
    if not workspace.path_is_within(full, root) then
        return nil, "wiki path escapes configured wiki directory"
    end
    local root_real = workspace.realpath(root)
    local full_real = workspace.realpath(full)
    if root_real and full_real and not workspace.path_is_within(full_real, root_real) then
        return nil, "wiki path resolves outside configured wiki directory"
    end
    return full, nil, root
end

local function mkdir_p(path)
    local quoted = workspace.shell_quote(path)
    local ok = os.execute("mkdir -p -- " .. quoted)
    return ok == true or ok == 0
end

local function write_all(path, content)
    local dir = workspace.dirname(path)
    if dir and dir ~= "." and not mkdir_p(dir) then
        return nil, "cannot create directory: " .. dir
    end
    local file, err = io.open(path, "wb")
    if not file then return nil, err end
    file:write(content or "")
    file:close()
    return true, nil
end

local function is_markdown(path)
    local lower = tostring(path or ""):lower()
    return lower:match("%.md$") ~= nil or lower:match("%.markdown$") ~= nil
end

local function list_markdown_files(path)
    local full = workspace.normalize_path(workspace.expand_home_path(path))
    local files = {}
    local real = workspace.realpath(full) or full
    local quoted = workspace.shell_quote(real)
    local cmd = "if [ -f " .. quoted .. " ]; then printf '%s\\n' " .. quoted .. "; elif [ -d " .. quoted .. " ]; then find " .. quoted .. " -type f \\( -name '*.md' -o -name '*.markdown' \\) -print 2>/dev/null; fi"
    local pipe = io.popen(cmd)
    if not pipe then return nil, "cannot scan source path" end
    for line in pipe:lines() do
        if is_markdown(line) then table.insert(files, workspace.normalize_path(line)) end
    end
    pipe:close()
    table.sort(files)
    if #files == 0 then return nil, "no Markdown files found" end
    local root = real
    if #files == 1 and files[1] == real then
        root = workspace.dirname(real)
    end
    return files, nil, workspace.normalize_path(root)
end

local function stable_hash(text)
    local h = 5381
    text = tostring(text or "")
    for i = 1, #text do
        h = (h * 33 + text:byte(i)) % 4294967296
    end
    return string.format("%08x", h)
end

local function source_id_for(root)
    local base = tostring(root or "source"):match("([^/]+)$") or "source"
    base = base:lower():gsub("[^a-z0-9]+", "-"):gsub("^-+", ""):gsub("-+$", "")
    if base == "" then base = "source" end
    return base .. "-" .. stable_hash(root)
end

local function relative_to_root(path, root)
    path = workspace.normalize_path(path)
    root = workspace.normalize_path(root):gsub("/+$", "")
    if path == root then return path:match("([^/]+)$") or path end
    if path:sub(1, #root + 1) == root .. "/" then
        return path:sub(#root + 2)
    end
    return path
end

local function strip_frontmatter(content)
    if type(content) ~= "string" or content:sub(1, 4) ~= "---\n" then return content or "" end
    local finish = content:find("\n---\n", 5, true)
    if not finish then return content end
    return content:sub(finish + 5)
end

local function yaml_scalar(value)
    value = tostring(value or "")
    value = value:gsub("\\", "\\\\"):gsub('"', '\\"')
    return '"' .. value .. '"'
end

local function yaml_list(values)
    local lines = {}
    if type(values) ~= "table" then return "" end
    for _, value in ipairs(values) do
        table.insert(lines, "  - " .. yaml_scalar(value))
    end
    return table.concat(lines, "\n")
end

local function frontmatter_for(entry)
    return table.concat({
        "---",
        "schema_version: 1",
        "id: " .. yaml_scalar(entry.id or ""),
        "kind: " .. yaml_scalar(entry.kind or "source"),
        "title: " .. yaml_scalar(entry.title or entry.path or ""),
        "description: " .. yaml_scalar(entry.description or ""),
        "use_when:",
        yaml_list(entry.use_when),
        "tags:",
        yaml_list(entry.tags),
        "index_policy: " .. yaml_scalar(entry.index_policy or "always"),
        "context_policy: " .. yaml_scalar(entry.context_policy or "retrieve_only"),
        "---",
        "",
    }, "\n")
end

local function compact_content(content, limit)
    content = content or ""
    if #content <= limit then return content end
    return content:sub(1, limit) .. "\n\n[truncated]"
end

local function sanitize_utf8(text)
    text = tostring(text or "")
    local out = {}
    local i = 1
    while i <= #text do
        local b1 = text:byte(i)
        if b1 == 9 or b1 == 10 or b1 == 13 or (b1 >= 32 and b1 <= 127) then
            table.insert(out, text:sub(i, i))
            i = i + 1
        elseif b1 >= 194 and b1 <= 223 and i + 1 <= #text then
            local b2 = text:byte(i + 1)
            if b2 >= 128 and b2 <= 191 then
                table.insert(out, text:sub(i, i + 1))
                i = i + 2
            else
                table.insert(out, "?")
                i = i + 1
            end
        elseif b1 >= 224 and b1 <= 239 and i + 2 <= #text then
            local b2 = text:byte(i + 1)
            local b3 = text:byte(i + 2)
            local valid = b3 >= 128 and b3 <= 191 and (
                (b1 == 224 and b2 >= 160 and b2 <= 191) or
                (b1 >= 225 and b1 <= 236 and b2 >= 128 and b2 <= 191) or
                (b1 == 237 and b2 >= 128 and b2 <= 159) or
                (b1 >= 238 and b1 <= 239 and b2 >= 128 and b2 <= 191)
            )
            if valid then
                table.insert(out, text:sub(i, i + 2))
                i = i + 3
            else
                table.insert(out, "?")
                i = i + 1
            end
        elseif b1 >= 240 and b1 <= 244 and i + 3 <= #text then
            local b2 = text:byte(i + 1)
            local b3 = text:byte(i + 2)
            local b4 = text:byte(i + 3)
            local valid = b3 >= 128 and b3 <= 191 and b4 >= 128 and b4 <= 191 and (
                (b1 == 240 and b2 >= 144 and b2 <= 191) or
                (b1 >= 241 and b1 <= 243 and b2 >= 128 and b2 <= 191) or
                (b1 == 244 and b2 >= 128 and b2 <= 143)
            )
            if valid then
                table.insert(out, text:sub(i, i + 3))
                i = i + 4
            else
                table.insert(out, "?")
                i = i + 1
            end
        else
            table.insert(out, "?")
            i = i + 1
        end
    end
    return table.concat(out)
end

local function source_snapshot(files, root)
    local docs = {}
    local remaining = 60000
    for _, file in ipairs(files) do
        local content = workspace.read_all(file) or ""
        local budget = 0
        if remaining > 0 then
            budget = math.min(12000, remaining)
            if budget < 1200 and #docs == 0 then budget = remaining end
        end
        remaining = math.max(0, remaining - budget)
        local clean = sanitize_utf8(strip_frontmatter(content))
        table.insert(docs, {
            path = relative_to_root(file, root),
            hash = stable_hash(content),
            content = budget > 0 and sanitize_utf8(compact_content(clean, budget)) or "",
        })
    end
    return docs
end

local function metadata_prompt(source_id, source_root, docs)
    return table.concat({
        "Create a compact Capstan wiki metadata index for these Markdown files.",
        "Return strict JSON only. Do not wrap it in Markdown.",
        "Schema:",
        '{"entries":[{"path":"relative.md","id":"stable-id","kind":"context|resource|page|source","title":"Short title","description":"One sentence","use_when":["When this file helps"],"tags":["tag"],"index_policy":"always","context_policy":"retrieve_only"}]}',
        "Use exactly the provided relative paths. Do not include file bodies.",
        "",
        json.encode({ source_id = source_id, source_root = source_root, files = docs }),
    }, "\n")
end

local function parse_json_object(text)
    text = tostring(text or ""):match("^%s*(.-)%s*$")
    if text:sub(1, 3) == "```" then
        text = text:gsub("^```[%w_%-]*%s*", ""):gsub("%s*```$", "")
    end
    local ok, decoded = pcall(json.decode, text)
    if ok and type(decoded) == "table" then
        return decoded, nil
    end
    local first = text:find("{", 1, true)
    local last = text:match("^.*()}")
    if first and last and last >= first then
        local candidate = text:sub(first, last)
        ok, decoded = pcall(json.decode, candidate)
        if ok and type(decoded) == "table" then
            return decoded, nil
        end
    end
    return nil, tostring(decoded)
end

local function sanitize_entry(raw, known_paths, source_id)
    if type(raw) ~= "table" then return nil end
    local path = tostring(raw.path or "")
    if path == "" or not known_paths[path] or invalid_relative(path) then return nil end
    local function array(name)
        local result = {}
        if type(raw[name]) == "table" then
            for _, value in ipairs(raw[name]) do
                if type(value) == "string" and value ~= "" then table.insert(result, value) end
            end
        end
        return result
    end
    local id = tostring(raw.id or "")
    if id == "" then id = source_id .. ":" .. path end
    return {
        path = path,
        id = id,
        kind = tostring(raw.kind or "source"),
        title = tostring(raw.title or path),
        description = tostring(raw.description or ""),
        use_when = array("use_when"),
        tags = array("tags"),
        index_policy = tostring(raw.index_policy or "always"),
        context_policy = tostring(raw.context_policy or "retrieve_only"),
    }
end

local function index_path(root, source_id)
    return root:gsub("/+$", "") .. "/index/" .. source_id .. ".json"
end

local function save_index(root, source_id, source_root, docs, decoded, mode, target_dir)
    local known = {}
    for _, doc in ipairs(docs) do known[doc.path] = doc end
    local entries = {}
    local raw_entries = type(decoded.entries) == "table" and decoded.entries or {}
    for _, raw in ipairs(raw_entries) do
        local entry = sanitize_entry(raw, known, source_id)
        if entry then
            entry.hash = known[entry.path] and known[entry.path].hash or nil
            if mode == "copy" then
                entry.wiki_path = (target_dir:gsub("/+$", "") .. "/" .. entry.path):gsub("^/+", "")
            end
            table.insert(entries, entry)
        end
    end
    if #entries == 0 then
        for _, doc in ipairs(docs) do
            table.insert(entries, sanitize_entry({ path = doc.path, title = doc.path }, known, source_id))
        end
    end
    local payload = {
        schema_version = 1,
        source_id = source_id,
        source_root = source_root,
        indexed_at = os.date("!%Y-%m-%dT%H:%M:%SZ"),
        generated_by = "wiki-ingest",
        mode = mode or "external",
        target_dir = target_dir,
        entries = entries,
    }
    return write_all(index_path(root, source_id), json.encode(payload))
end

local function save_fallback_index(root, source_id, source_root, docs, mode, target_dir, reason)
    return save_index(root, source_id, source_root, docs, { entries = {} }, mode, target_dir), reason
end

local function copy_to_wiki(root, source_root, files, entries, target_dir)
    target_dir = (target_dir or "sources"):gsub("^/+", ""):gsub("/+$", "")
    if target_dir == "" or workspace.is_absolute_path(target_dir) or target_dir:find("..", 1, true) then
        return nil, "copy target must be a relative wiki directory"
    end
    local by_path = {}
    for _, entry in ipairs(entries or {}) do by_path[entry.path] = entry end
    for _, file in ipairs(files) do
        local rel = relative_to_root(file, source_root)
        local dest = workspace.normalize_path(root .. "/" .. target_dir .. "/" .. rel)
        if not workspace.path_is_within(dest, root) then
            return nil, "copy target escapes wiki directory"
        end
        local content, err = workspace.read_all(file)
        if not content then return nil, err or ("cannot read " .. file) end
        local entry = by_path[rel] or { path = rel, title = rel, kind = "source", index_policy = "always", context_policy = "retrieve_only" }
        local ok, write_err = write_all(dest, frontmatter_for(entry) .. strip_frontmatter(content))
        if not ok then return nil, write_err end
    end
    return true, nil
end

local function run_weak_index(root, source_id, source_root, files, docs, mode, target_dir)
    if not capstan or type(capstan.agent) ~= "table" or type(capstan.agent.run) ~= "function" then
        return nil, "agent runtime is not available"
    end
    local opts = {
        messages = {{ role = "user", content = metadata_prompt(source_id, source_root, docs) }},
        tools = {},
        max_turns = 1,
        update_status = false,
        update_usage = false,
    }
    if capstan.models and type(capstan.models.weak) == "function" then
        local weak = capstan.models.weak()
        if type(weak) == "table" then
            opts.provider = weak.provider
            opts.model = weak.model
        end
    end
    local chunks = {}
    local state = {
        done = false,
        ok = false,
        error = nil,
        fallback = false,
    }
    local function finish_with_fallback(reason)
        local save_ok, save_err = save_fallback_index(root, source_id, source_root, docs, mode, target_dir, reason)
        if not save_ok then
            state.error = tostring(save_err)
            state.done = true
            notify_error("Wiki ingest failed: " .. tostring(save_err))
            return
        end
        state.fallback = true
        state.ok = true
        state.done = true
        notify_info("Wiki ingest indexed " .. tostring(#docs) .. " file(s) with fallback metadata")
        log("fallback metadata index: " .. tostring(reason))
    end
    local ok, err = capstan.agent.run(opts, {
        on_text = function(chunk) table.insert(chunks, chunk or "") end,
        on_error = function(message)
            finish_with_fallback(tostring(message or "model error"))
        end,
        on_done = function(result)
            if state.done then return end
            if result and result.ok == false then
                finish_with_fallback(tostring(result.error or state.error or "model error"))
                return
            end
            local text = result and result.text
            if type(text) ~= "string" or text == "" then text = table.concat(chunks) end
            local decoded, decode_err = parse_json_object(text)
            if not decoded then
                finish_with_fallback("invalid metadata JSON")
                log("invalid metadata JSON: " .. tostring(decode_err))
                return
            end
            local save_ok, save_err = save_index(root, source_id, source_root, docs, decoded, mode, target_dir)
            if not save_ok then
                state.error = tostring(save_err)
                state.done = true
                notify_error("Wiki ingest failed: " .. tostring(save_err))
                return
            end
            if mode == "copy" then
                local entries = decoded.entries or {}
                local copy_ok, copy_err = copy_to_wiki(root, source_root, files, entries, target_dir)
                if not copy_ok then
                    state.error = tostring(copy_err)
                    state.done = true
                    notify_error("Wiki ingest copy failed: " .. tostring(copy_err))
                    return
                end
            end
            state.ok = true
            state.done = true
            notify_info("Wiki ingest indexed " .. tostring(#docs) .. " file(s)")
        end,
    })
    if not ok then return nil, err end
    while not state.done do
        if not http or type(http.poll) ~= "function" then
            return nil, "http.poll is not available while waiting for wiki ingest"
        end
        http.poll()
        if type(http.wait_frame) == "function" then
            http.wait_frame()
        end
    end
    if not state.ok then
        return nil, state.error or "wiki ingest failed"
    end
    return true, state.fallback and "fallback" or nil
end

local function ingest_options(options)
    local root = wiki_root()
    if not root then
        return "Wiki ingest failed: Wiki is not configured. Set wiki.path in ~/.config/capstan/config.lua."
    end
    options = options or {}
    local copy = options.copy == true
    local source_path = options.path
    if type(source_path) ~= "string" or source_path == "" then
        return "Wiki ingest failed: path is required"
    end
    local files, scan_err, source_root = list_markdown_files(source_path)
    if not files then return "Wiki ingest failed: " .. tostring(scan_err) end
    local source_id = source_id_for(source_root)
    local docs = source_snapshot(files, source_root)
    local mode = copy and "copy" or "external"
    local target_dir = nil
    if copy then
        target_dir = options.target_dir or ("sources/" .. source_id)
        if invalid_relative(target_dir) or target_dir:find("..", 1, true) then
            return "Wiki ingest failed: copy target must be a relative wiki directory"
        end
    end
    local ok, err = run_weak_index(root, source_id, source_root, files, docs, mode, target_dir)
    if not ok then return "Wiki ingest failed: " .. tostring(err) end
    local suffix = err == "fallback" and " with fallback metadata" or ""
    return "Wiki ingest completed" .. suffix .. ": " .. tostring(#files) .. " Markdown file(s) from " .. source_root .. " using source `" .. source_id .. "`."
end

local function ingest(args)
    local copy = false
    local path_index = 2
    if args[2] == "--copy" then
        copy = true
        path_index = 3
    end
    local source_path = args[path_index]
    if type(source_path) ~= "string" or source_path == "" then
        return "Usage: /wiki ingest [--copy] <markdown-file-or-directory> [target-dir]"
    end
    return ingest_options({
        path = source_path,
        copy = copy,
        target_dir = copy and args[path_index + 1] or nil,
    })
end

local function onboarding_message()
    local default_path = "~/.local/state/capstan/wiki"
    return table.concat({
        "Capstan Wiki is not initialized yet.",
        "",
        "Use the built-in `wiki-onboarding` skill to guide the user conversationally.",
        "The default internal location is `" .. default_path .. "`; do not ask for read permission inside it.",
        "Ask what stable owner preferences should go into `profile/core.md` and whether to create starter folders.",
        "Only discuss or set `wiki.path` when the user explicitly wants a custom location.",
    }, "\n")
end

local function status()
    if not wiki_config() then
        return onboarding_message()
    end
    if capstan and type(capstan.wiki_summary) == "string" and capstan.wiki_summary ~= "" then
        return capstan.wiki_summary
    end
    return "Wiki is not configured. Set wiki.path in ~/.config/capstan/config.lua."
end

local function read_index_file(path)
    local content = workspace.read_all(path)
    if not content then return nil end
    local ok, decoded = pcall(json.decode, content)
    if ok and type(decoded) == "table" then return decoded end
    return nil
end

local function source_index(source_id)
    local root = wiki_root()
    if not root then return nil, "Wiki is not configured. Set wiki.path in ~/.config/capstan/config.lua." end
    if type(source_id) ~= "string" or source_id == "" or source_id:find("/", 1, true) then
        return nil, "invalid source id"
    end
    local index = read_index_file(index_path(root, source_id))
    if not index then return nil, "source is not indexed: " .. source_id end
    return index, nil
end

local function source_read(args)
    local source = args and args.source
    local rel = args and args.path
    if invalid_relative(rel) then return "Wiki source read failed: path must be relative", false end
    local index, err = source_index(source)
    if not index then return "Wiki source read failed: " .. tostring(err), false end
    local source_root = index.source_root
    if type(source_root) ~= "string" or source_root == "" then
        return "Wiki source read failed: index has no source_root", false
    end
    local allowed = false
    for _, entry in ipairs(index.entries or {}) do
        if type(entry) == "table" and entry.path == rel then
            allowed = true
            break
        end
    end
    if not allowed then
        return "Wiki source read failed: path is not present in source index", false
    end
    local full = workspace.normalize_path(source_root .. "/" .. rel)
    if not workspace.path_is_within(full, workspace.normalize_path(source_root)) then
        return "Wiki source read failed: path escapes source root", false
    end
    local content, read_err = workspace.read_all(full)
    if not content then
        return "Wiki source read failed: " .. tostring(read_err or "cannot read file"), false
    end
    return "Wiki source file: " .. tostring(source) .. ":" .. rel .. "\n\n" .. content, true
end

function plugin.handler(ctx)
    if ctx.tool_name == "wiki_source_read" then
        local result, ok = source_read(ctx.tool_args or {})
        if not ok then return ctx:error(result) end
        return ctx:replace(result)
    end
    if ctx.tool_name == "wiki_ingest" then
        return ctx:replace(ingest_options(ctx.tool_args or {}))
    end

    local tool_path = ctx.tool_args and ctx.tool_args.path
    local subcommand = tool_path and "read" or ctx.args[1]

    if subcommand == nil or subcommand == "status" then
        return ctx:replace(status())
    end

    if subcommand == "ingest" then
        return ctx:replace(ingest(ctx.args or {}), "")
    end

    if subcommand ~= "read" then
        return ctx:replace("Usage: /wiki status | /wiki read <relative-path> | /wiki ingest [--copy] <markdown-file-or-directory> [target-dir]")
    end

    local relative = tool_path or ctx.args[2]
    local full, err = resolve_wiki_path(relative)
    if not full then
        if tool_path then return ctx:error("Wiki read failed: " .. err) end
        return ctx:replace("Wiki read failed: " .. err)
    end

    local content, read_err = workspace.read_all(full)
    if not content then
        local message = "Wiki read failed: " .. tostring(read_err or "cannot read file")
        if tool_path then return ctx:error(message) end
        return ctx:replace(message)
    end

    local label = "Wiki file: " .. relative .. "\n\n"
    return ctx:replace(label .. content)
end

return plugin
