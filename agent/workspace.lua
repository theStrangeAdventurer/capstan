local M = {}

local UTF8_BOM = string.char(0xef, 0xbb, 0xbf)

-- Embedded references are exact in-memory asset names, not filesystem paths.
-- Keep even an empty name in this namespace so invalid references fail there.
function M.embedded_asset_name(path)
    if type(path) ~= "string" then return nil end
    return path:match("^embedded:(.*)$")
end

-- One ordering/deduplication policy for file-read execution and permissions.
function M.file_read_paths(args)
    local paths, seen = {}, {}
    local function append(path)
        if type(path) ~= "string" or path == "" or seen[path] then return end
        seen[path] = true
        paths[#paths + 1] = path
    end
    if type(args.paths) == "table" then
        for _, path in ipairs(args.paths) do append(path) end
    end
    append(args.path)
    return paths
end

function M.is_absolute_path(path)
    return type(path) == "string" and path:sub(1, 1) == "/"
end

function M.configured_workdir()
    if _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir ~= "" and M.is_absolute_path(_G.capstan.workdir) then
        return _G.capstan.workdir
    end
    local env = os.getenv("CAPSTAN_WORKDIR") or os.getenv("CAPSTAN_WORKSPACE")
    if env and env ~= "" and M.is_absolute_path(env) then
        return env
    end
    local pwd = os.getenv("PWD")
    if pwd and pwd ~= "" and M.is_absolute_path(pwd) then
        return pwd
    end
    return "."
end

function M.runtime_workdir()
    if _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir ~= "" then
        return _G.capstan.workdir
    end
    return nil
end

function M.configured_workspace_root()
    if _G.capstan and type(_G.capstan.workspace_root) == "string" and
        _G.capstan.workspace_root ~= "" and M.is_absolute_path(_G.capstan.workspace_root) then
        return _G.capstan.workspace_root
    end
    return M.configured_workdir()
end

function M.wiki_enabled()
    local options = _G.capstan and _G.capstan.runtime_options
    return not (type(options) == "table" and options.disable_wiki == true)
end

function M.configured_wiki_path()
    if not _G.capstan then return nil end
    if not M.wiki_enabled() then return nil end
    if type(_G.capstan.config) == "table" and type(_G.capstan.config.wiki) == "table" then
        local path = _G.capstan.config.wiki.path
        if type(path) == "string" and path ~= "" then return path end
    end
    if type(_G.capstan.wiki_path) == "string" and _G.capstan.wiki_path ~= "" then
        return _G.capstan.wiki_path
    end
    if type(_G.capstan.state_path) == "function" then
        return _G.capstan.state_path("wiki")
    end
    return nil
end

function M.configured_wiki_root()
    local configured = M.configured_wiki_path()
    if not configured then return nil end
    return M.normalize_path(M.expand_home_path(configured))
end

function M.expand_home_path(path)
    if type(path) ~= "string" then return path end
    if path ~= "~" and path:sub(1, 2) ~= "~/" then return path end
    local home = os.getenv("HOME")
    if not home or home == "" then return path end
    return home .. path:sub(2)
end

function M.resolve_path(path)
    if M.is_absolute_path(path) then
        return path
    end
    return M.configured_workdir():gsub("/+$", "") .. "/" .. path
end

function M.realpath(path)
    if _G.capstan and type(_G.capstan.realpath) == "function" then
        local ok, resolved = pcall(_G.capstan.realpath, path)
        if ok then return resolved end
    end
    return nil
end

function M.path_is_within(path, base)
    if type(path) ~= "string" or type(base) ~= "string" or path == "" or base == "" then
        return false
    end
    if path ~= "/" then path = path:gsub("/+$", "") end
    if base ~= "/" then base = base:gsub("/+$", "") end
    if base == "/" then return path:sub(1, 1) == "/" end
    return path == base or path:sub(1, #base + 1) == base .. "/"
end

local function nearest_existing_parent(path)
    local dir = M.dirname(path)
    while dir and dir ~= "" do
        local real = M.realpath(dir)
        if real then return real, dir end
        if dir == "." or dir == "/" then break end
        local next_dir = M.dirname(dir)
        if next_dir == dir then break end
        dir = next_dir
    end
    return nil, dir
end

function M.real_workspace()
    return M.realpath(M.configured_workspace_root())
end

local function requested_path_is_within_workspace(path)
    local requested = M.normalize_path(path)
    local configured = M.normalize_path(M.configured_workspace_root())
    return M.path_is_within(requested, configured)
end

local function real_skill_roots()
    local roots = {}
    local configured = _G.capstan and _G.capstan.skill_roots
    if type(configured) ~= "table" then return roots end
    for _, root in ipairs(configured) do
        if type(root) == "string" and root ~= "" then
            local real = M.realpath(root)
            if real then table.insert(roots, real) end
        end
    end
    return roots
end

local function path_is_allowed_skill_read(requested_path, target_real)
    if type(requested_path) ~= "string" or requested_path == "" then return false end
    local requested = M.normalize_path(requested_path)
    for _, root in ipairs(real_skill_roots()) do
        if M.path_is_within(target_real, root) then
            return true
        end
    end
    local configured = _G.capstan and _G.capstan.skill_roots
    if type(configured) ~= "table" then return false end
    for _, root in ipairs(configured) do
        if type(root) == "string" and root ~= "" then
            local normalized_root = M.normalize_path(root)
            if M.path_is_within(requested, normalized_root) then
                return true
            end
        end
    end
    return false
end

-- Resolve creation targets through existing ancestors without following dangling
-- links. Shared by Wiki copies and model writes; not an authorization bypass.
function M.creation_realpath(path)
    local real = M.realpath(path)
    if real then return real end
    local quoted = M.shell_quote(path)
    local absent = os.execute("[ ! -e " .. quoted .. " ] && [ ! -L " .. quoted .. " ]")
    if absent ~= true and absent ~= 0 then return nil end
    local parent = M.dirname(path)
    if parent == path or path == "/" or path == "." then return nil end
    local parent_real = M.creation_realpath(parent)
    if not parent_real then return nil end
    return M.normalize_path(parent_real .. "/" .. (path:match("([^/]+)$") or ""))
end

function M.model_path_allowed(path, mode, opts)
    if not (_G.capstan and type(_G.capstan.realpath) == "function") then
        return true
    end

    local resolved = M.resolve_path(path)
    if mode == "write" and opts and opts.allow_wiki_write then
        local root = M.configured_wiki_root()
        if root and M.path_is_within(M.normalize_path(resolved), root) then
            local root_real = M.creation_realpath(root)
            local target_real = M.creation_realpath(resolved)
            if root_real and target_real and M.path_is_within(target_real, root_real) then
                return true
            end
            return false, "resolved path escapes configured wiki directory or cannot be resolved"
        end
    end

    local workdir = M.real_workspace()
    if not workdir then
        return false, "workspace realpath failed"
    end
    local target_real = M.realpath(resolved)
    if target_real then
        if M.path_is_within(target_real, workdir) then
            return true
        end
        if mode == "read" and path_is_allowed_skill_read(resolved, target_real) then
            return true
        end
        local requested = M.normalize_path(resolved)
        if mode == "read" and opts and opts.allow_outside_workspace and
            not requested_path_is_within_workspace(requested) then
            return true
        end
        return false, "resolved path escapes workspace: " .. target_real
    end

    if mode == "write" then
        local parent_real = nearest_existing_parent(resolved)
        if parent_real and M.path_is_within(parent_real, workdir) then
            return true
        end
        return false, "parent directory escapes workspace"
    end

    return false, "path does not exist"
end

function M.is_sensitive_path(path)
    if type(path) ~= "string" then return false end
    local name = path:match("([^/]+)$") or path
    local lower = name:lower()
    return lower == ".env" or lower:match("^%.env%.") ~= nil or
        lower:find("secret", 1, true) ~= nil or
        lower:find("token", 1, true) ~= nil or
        lower:find("credential", 1, true) ~= nil
end

function M.normalize_path(path, workdir)
    if type(path) ~= "string" or path == "" then
        return path
    end

    path = M.expand_home_path(path)
    if not M.is_absolute_path(path) then
        local base = workdir
        if not base or base == "" then
            return path
        end
        path = base:gsub("/+$", "") .. "/" .. path
    end

    local parts = {}
    for part in path:gmatch("[^/]+") do
        if part == "." then
        elseif part == ".." then
            if #parts > 0 then
                table.remove(parts)
            end
        else
            table.insert(parts, part)
        end
    end

    return "/" .. table.concat(parts, "/")
end

-- Returns the safe relative Wiki path for a local path, or nil when the path
-- is not inside the configured Wiki. This lets the tool dispatcher preserve
-- the Wiki's permission-free internal-read policy even if the model selected
-- the generic file reader.
function M.wiki_relative_path(path)
    if type(path) ~= "string" or path == "" or M.embedded_asset_name(path) then return nil end
    local root = M.configured_wiki_root()
    if not root then return nil end
    local full = M.normalize_path(path, M.runtime_workdir() or M.configured_workdir())
    if not M.path_is_within(full, root) or full == root then return nil end

    local root_real = M.realpath(root)
    local full_real = M.realpath(full)
    if root_real and full_real and not M.path_is_within(full_real, root_real) then
        return nil
    end

    return full:sub(#root:gsub("/+$", "") + 2)
end

-- Only a lexical check, not a shell interpreter or an OS sandbox. Keep quoted
-- and escaped operators in words; real operators delimit words without spaces.
local function shell_scope_tokens(command)
    local tokens, word, heredocs = {}, {}, {}
    local started, quoted, quote = false, false, nil
    local function flush()
        if started then
            local value = table.concat(word)
            local previous = tokens[#tokens]
            if previous and previous.kind == "redirect" and
                (previous.value == "<<" or previous.value == "<<-") then
                heredocs[#heredocs + 1] = {
                    delimiter = value, strip_tabs = previous.value == "<<-", literal = quoted,
                }
            end
            tokens[#tokens + 1] = { kind = "word", value = value }
        end
        word, started, quoted = {}, false, false
    end
    local i = 1
    while i <= #command do
        local c, next_c = command:sub(i, i), command:sub(i + 1, i + 1)
        if quote == "'" then
            if c == quote then quote = nil else word[#word + 1] = c end
        elseif c == "\\" then
            if next_c == "" then return nil, "incomplete shell escape" end
            if next_c == "\n" then
                i = i + 1
            elseif not quote or next_c:match('[\\$`"]') then
                word[#word + 1], started, quoted = next_c, true, true
                i = i + 1
            else
                word[#word + 1] = c
            end
        elseif c == "`" or (c == "$" and next_c == "(") then
            return nil, "dynamic home or command substitution is outside workspace policy"
        elseif quote then
            if c == quote then quote = nil else word[#word + 1] = c end
        elseif c == "'" or c == '"' then
            quote, started, quoted = c, true, true
        elseif c == "<" or c == ">" or (c == "&" and next_c == ">") then
            -- An unquoted number immediately before an operator is an IO fd,
            -- not a command or argument (2>file, but not '2'>file).
            if not quoted and table.concat(word):match("^%d+$") then
                word, started = {}, false
            end
            flush()
            local op = c
            if next_c == c or next_c == "&" or
                (c == "<" and next_c == ">") or
                (c == ">" and next_c == "|") or
                (c == "&" and next_c == ">") then
                op, i = op .. next_c, i + 1
            end
            local third = command:sub(i + 1, i + 1)
            if (op == "<<" and (third == "-" or third == "<")) or
                (op == "&>" and third == ">") then
                op, i = op .. third, i + 1
            end
            tokens[#tokens + 1] = { kind = "redirect", value = op }
        elseif c:match("[;|&()\n]") then
            flush()
            tokens[#tokens + 1] = { kind = "separator", value = c }
            if c == "\n" then
                -- Heredoc bodies are data, not shell words or redirections.
                for _, doc in ipairs(heredocs) do
                    local found = false
                    while i < #command do
                        local eol = command:find("\n", i + 1, true) or (#command + 1)
                        local line = command:sub(i + 1, eol - 1)
                        if doc.strip_tabs then line = line:gsub("^\t+", "") end
                        i = eol
                        if line == doc.delimiter then found = true; break end
                        if not doc.literal and (line:find("$(", 1, true) or line:find("`", 1, true)) then
                            return nil, "command substitution in heredoc is outside workspace policy"
                        end
                    end
                    if not found then return nil, "unterminated shell heredoc" end
                end
                heredocs = {}
            end
        elseif c:match("%s") then
            flush()
        else
            word[#word + 1], started = c, true
        end
        i = i + 1
    end
    if quote then return nil, "unterminated shell quote" end
    flush()
    if #heredocs > 0 then return nil, "unterminated shell heredoc" end
    return tokens
end

local function shell_token_value(token)
    return token:match("^[%w_]+=(.+)$") or token
end

local function shell_path_candidate(token)
    token = shell_token_value(token)
    if token == "/dev/null" then return nil end
    if token:find("$HOME", 1, true) or token:find("${HOME}", 1, true) or
        token:find("$(", 1, true) or token:find("`", 1, true) then
        return false
    end
    if token:sub(1, 1) == "/" or token:sub(1, 2) == "~/" or
        token == ".." or token:sub(1, 3) == "../" or
        token:find("/../", 1, true) then
        return token
    end
    return nil
end

function M.shell_command_within_workspace(command)
    if type(command) ~= "string" or command == "" then
        return false, "empty shell command"
    end
    local root = M.normalize_path(M.configured_workspace_root())
    if root ~= "/" then root = root:gsub("/+$", "") end
    local workdir = M.normalize_path(M.configured_workdir())
    local current_dir = workdir
    local command_position = true
    local command_name = nil
    local cd_target_seen = false
    local function finish_command()
        if command_name == "cd" and not cd_target_seen then
            return false, "cd requires a static workspace path"
        end
        command_position = true
        command_name = nil
        cd_target_seen = false
        return true
    end
    local tokens, token_err = shell_scope_tokens(command)
    if not tokens then return false, token_err end
    local redirect = nil
    for _, item in ipairs(tokens) do
        local token = item.value
        if item.kind ~= "word" and redirect then
            return false, "missing shell redirection target"
        end
        if item.kind == "separator" then
            local ok, err = finish_command()
            if not ok then return false, err end
        elseif item.kind == "redirect" then
            redirect = token
        else
            local candidate = shell_path_candidate(token)
            if candidate == false then
                return false, "dynamic home or command substitution is outside workspace policy"
            end
            if redirect then
                -- Heredoc delimiters, here-strings and numeric fd duplication
                -- operands are not filenames. Other targets consume one word
                -- without changing the command position (even before `cd`).
                local is_data = redirect == "<<" or redirect == "<<-" or redirect == "<<<"
                local is_fd = (redirect == ">&" or redirect == "<&") and
                    (token:match("^%d+%-?$") or token == "-")
                if not is_data and not is_fd and token ~= "/dev/null" then
                    local resolved = M.normalize_path(token, current_dir)
                    if token == "" or not M.path_is_within(resolved, root) then
                        return false, "shell path escapes workspace: " .. resolved
                    end
                end
                redirect = nil
            elseif command_position then
                command_name = shell_token_value(token)
                command_position = false
            elseif command_name == "cd" and not cd_target_seen then
                local target = shell_token_value(token)
                if target == "--" or target:match("^%-[eLP]+$") then
                    -- cd options precede the statically visible target.
                elseif target == "-" or target == "" or target:find("$", 1, true) or
                    target:find("`", 1, true) or target:find("*", 1, true) or
                    target:find("?", 1, true) then
                    return false, "dynamic cd target is outside workspace policy"
                else
                    local resolved = M.normalize_path(target, current_dir)
                    if resolved ~= "/" then resolved = resolved:gsub("/+$", "") end
                    if not M.path_is_within(resolved, root) then
                        return false, "shell path escapes workspace: " .. resolved
                    end
                    current_dir = resolved
                    cd_target_seen = true
                end
            elseif candidate then
                local resolved = M.normalize_path(candidate, current_dir)
                if resolved ~= "/" then resolved = resolved:gsub("/+$", "") end
                if not M.path_is_within(resolved, root) then
                    return false, "shell path escapes workspace: " .. resolved
                end
            end
        end
    end
    if redirect then return false, "missing shell redirection target" end
    local ok, err = finish_command()
    if not ok then return false, err end
    return true
end

function M.collapse_home_path(path)
    if type(path) ~= "string" or path == "" then return path end
    local home = os.getenv("HOME")
    if not home or home == "" then return path end
    home = home:gsub("/+$", "")
    if path == home then return "~" end
    if path:sub(1, #home + 1) == home .. "/" then
        return "~" .. path:sub(#home + 1)
    end
    return path
end

function M.shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

function M.read_all(path)
    local file, err = io.open(path, "rb")
    if not file then
        return nil, err
    end
    local content = file:read("*a") or ""
    file:close()
    return content, nil
end

function M.split_utf8_bom(content)
    content = content or ""
    if content:sub(1, #UTF8_BOM) == UTF8_BOM then
        return true, content:sub(#UTF8_BOM + 1)
    end
    return false, content
end

function M.utf8_bom()
    return UTF8_BOM
end

function M.dirname(path)
    local dir = path:match("^(.*)/[^/]*$")
    if not dir or dir == "" then
        return "."
    end
    return dir
end

function M.line_count(content)
    if content == "" then
        return 0
    end
    local lines = 1
    for _ in content:gmatch("\n") do
        lines = lines + 1
    end
    return lines
end

return M
