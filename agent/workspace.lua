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
