-- Protected JSON file; never copy managed keys into provider snapshots or history.
local json = require('vendor.rxi.json')
local oidc = require('agent.oidc')
local M = {}
local definitions = {
    deepseek = {label = "DeepSeek", env = "DEEPSEEK_API_KEY", endpoint = "https://api.deepseek.com/v1/chat/completions"},
    openrouter = {label = "OpenRouter", env = "OPENROUTER_API_KEY", endpoint = "https://openrouter.ai/api/v1/chat/completions"},
    opencode_go = {label = "OpenCode Go", env = "OPENCODE_GO_API_KEY", endpoint = "https://opencode.ai/inference/go/openai/v1/chat/completions"},
}
-- Remember precedence only, not keys: another process's updates/logout remain visible.
local prefer_file = {}
local function nonempty(value) return type(value) == "string" and value ~= "" and value or nil end
local function native()
    return _G.capstan and type(capstan.state_path) == 'function' and
        type(capstan.state_ensure_dir) == 'function' and capstan.oauth and
        type(capstan.oauth.store_open) == 'function'
end
local function fail(message) error({credential_error = message}, 0) end
local function valid_key(key)
    return nonempty(key) and #key <= 8192 and not key:find('%s') and not key:find('%c')
end
local function validate(value)
    if type(value) ~= 'table' or value.version ~= 1 or
        type(value.providers) ~= 'table' or value.providers == oidc.null then
        fail('Invalid credential file; existing contents were not replaced')
    end
    for id, record in pairs(value.providers) do
        if not definitions[id] or type(record) ~= 'table' or
            record.endpoint ~= definitions[id].endpoint or not valid_key(record.api_key) then
            fail('Invalid credential record; existing contents were not replaced')
        end
    end
    return value
end
local function storage(fn)
    if not native() then return nil, 'Credential storage unavailable' end
    local handle
    local ok, result = pcall(function()
        if not capstan.state_ensure_dir() then fail('Cannot create credential state directory') end
        local path = capstan.state_path('credentials')
        if not path then fail('Credential storage path unavailable') end
        handle = capstan.oauth.store_open(path)
        if not handle then fail('Unsafe or unavailable credential directory') end
        local clock = capstan.now_ms or function() return os.time() * 1000 end
        local deadline = clock() + 10000
        while true do
            local acquired = handle:try_lock()
            if acquired == nil then fail('Credential storage lock failed') end
            if acquired then break end
            if clock() >= deadline or not (http and type(http.wait_frame) == 'function') then
                fail('Credential storage is busy; retry later')
            end
            http.wait_frame()
        end
        local raw, err = handle:read()
        if err then fail('Credential file cannot be read safely') end
        local value = raw == nil and {version = 1, providers = {}} or validate(oidc.decode_json(raw))
        return fn(value, function()
            if not handle:write(json.encode(value)) then
                fail('Credential write failed; stored state could not be confirmed')
            end
        end)
    end)
    if handle then pcall(handle.close, handle) end
    if not ok then
        return nil, type(result) == 'table' and result.credential_error or 'Credential storage operation failed'
    end
    return result
end
local function stored_key(id)
    if not native() then return nil end
    local key, err = storage(function(value)
        return value.providers[id] and value.providers[id].api_key
    end)
    if err then error(err, 0) end
    return key
end
function M.definition(id) return definitions[id] end
function M.list()
    return {{id = "deepseek", label = "DeepSeek"}, {id = "openrouter", label = "OpenRouter"},
        {id = "opencode_go", label = "OpenCode Go"}}
end
local function identify(id, provider)
    if definitions[id] then return id end
    if definitions[provider.credential_provider] then return provider.credential_provider end
    for name, def in pairs(definitions) do if provider.endpoint == def.endpoint then return name end end
end
function M.resolve(id, provider, endpoint)
    provider = provider or {}
    id = identify(id, provider)
    local def = id and definitions[id]
    local canonical = def and provider.endpoint == def.endpoint and (not endpoint or endpoint == def.endpoint or
        endpoint == def.endpoint:gsub('/chat/completions$', '/models'))
    if canonical and prefer_file[id] then
        local key = stored_key(id)
        if key then return key, 'file (current connection)' end
        prefer_file[id] = nil
    end
    local env = def and nonempty(os.getenv(def.env))
    if env then return env, "environment" end
    if nonempty(provider.api_key) then return provider.api_key, "configuration" end
    if not canonical then return nil end
    local key = stored_key(id)
    return key, key and 'file' or nil
end
function M.source(id)
    if not native() then return 'not checked (connect to configure)' end
    local config = _G.capstan and capstan.config
    local provider = config and config.providers and config.providers[id] or {}
    local ok, _, source = pcall(M.resolve, id, {
        endpoint = provider.endpoint or definitions[id].endpoint, api_key = provider.api_key})
    if not ok then return 'credential file unavailable; check storage permissions and format' end
    return source or 'not connected'
end
function M.connect(id)
    local def = definitions[id]
    if not (_G.popup and type(popup.secret) == "function") then return "Secret input unavailable", false end
    local key = popup.secret(def.label, "Enter API key (never stored in command history)")
    if not nonempty(key) then return "Connection cancelled" end
    if not valid_key(key) then return 'Invalid API key: empty, oversized or contains whitespace/control characters', false end
    local ok, err = storage(function(value, save)
        value.providers[id] = {endpoint = def.endpoint, api_key = key}
        save()
        return true
    end)
    if not ok then return 'Key save failed: ' .. err, false end
    prefer_file[id] = true
    local message = "Connected: " .. id .. " — file (current connection)"
    if nonempty(os.getenv(def.env)) then message = message .. "; environment overrides stored keys on future launches" end
    return message
end
function M.logout(id)
    local ok, err = storage(function(value, save)
        if value.providers[id] then value.providers[id] = nil; save() end
        return true
    end)
    if not ok then return 'Logout failed: ' .. err, false end
    prefer_file[id] = nil
    return 'Stored key removed: ' .. id .. '. Environment/configuration keys are unchanged; remove them yourself.', true
end
return M
