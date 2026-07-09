local serialize = require("agent.lua_serialize")

local M = {}

local function auth_path()
    if not (_G.capstan and _G.capstan.state_path) then return nil end
    return _G.capstan.state_path("state/auth.lua")
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function ensure_auth_dir()
    if not (_G.capstan and _G.capstan.state_ensure_dir and _G.capstan.state_path) then
        return false, "state path is not available"
    end
    if not _G.capstan.state_ensure_dir() then
        return false, "cannot create state directory"
    end
    local dir = _G.capstan.state_path("state")
    if not dir then return false, "cannot resolve auth state directory" end
    local ok = os.execute("mkdir -p " .. shell_quote(dir))
    if ok == true or ok == 0 then return true end
    return false, "cannot create auth state directory"
end

local function load_all()
    local path = auth_path()
    if not path then return {} end
    local chunk = loadfile(path)
    if not chunk then return {} end
    local ok, value = pcall(chunk)
    if not ok or type(value) ~= "table" then return {} end
    return value
end

local function save_all(all)
    local ok, err = ensure_auth_dir()
    if not ok then return false, err end
    local path = auth_path()
    if not path then return false, "cannot resolve auth path" end
    if not (_G.capstan and type(_G.capstan.secure_write_file) == "function") then
        return false, "secure file writer is not available"
    end
    local write_ok, write_err = _G.capstan.secure_write_file(path, serialize.encode_return(all))
    if not write_ok then return false, write_err end
    return true
end

function M.get(provider_id)
    if type(provider_id) ~= "string" or provider_id == "" then return nil end
    local value = load_all()[provider_id]
    return type(value) == "table" and value or nil
end

function M.set(provider_id, credential)
    if type(provider_id) ~= "string" or provider_id == "" then
        return false, "missing provider id"
    end
    if type(credential) ~= "table" then
        return false, "missing credential"
    end
    local all = load_all()
    all[provider_id] = credential
    return save_all(all)
end

function M.remove(provider_id)
    local all = load_all()
    all[provider_id] = nil
    return save_all(all)
end

function M.list()
    return load_all()
end

local function redacted_token(value)
    if type(value) ~= "string" or value == "" then return nil end
    if #value <= 10 then return "<redacted>" end
    return value:sub(1, 4) .. "..." .. value:sub(-4)
end

function M.redacted(provider_id)
    local cred = M.get(provider_id)
    if not cred then return nil end
    local out = {
        type = cred.type,
        access = redacted_token(cred.access),
        refresh = redacted_token(cred.refresh),
        expires = cred.expires,
        metadata = cred.metadata,
    }
    return out
end

if not _G.capstan then _G.capstan = {} end
_G.capstan.auth = M

return M
