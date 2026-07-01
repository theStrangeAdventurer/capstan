local M = {}

local UTF8_BOM = string.char(0xef, 0xbb, 0xbf)

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
    path = path:gsub("/+$", "")
    base = base:gsub("/+$", "")
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
    return M.realpath(M.configured_workdir())
end

local function requested_path_is_within_workspace(path)
    local requested = M.normalize_path(path)
    local configured = M.normalize_path(M.configured_workdir())
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

function M.model_path_allowed(path, mode, opts)
    if not (_G.capstan and type(_G.capstan.realpath) == "function") then
        return true
    end

    local workdir = M.real_workspace()
    if not workdir then
        return false, "workspace realpath failed"
    end

    local resolved = M.resolve_path(path)
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
