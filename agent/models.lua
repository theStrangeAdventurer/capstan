local json = require("vendor.rxi.json")
local profiles = require("agent.profiles")
local state = require("agent.state")

local M = {}

local models_cache_by_url = {}

function M.model_context_length(item)
    local top = item.top_provider and item.top_provider.context_length or 0
    if top and top > 0 then return top end
    if item.context_length and item.context_length > 0 then
        return item.context_length
    end
    return 0
end

local function normalized_model_id(value)
    if not value then return "" end
    return tostring(value):lower():gsub("[^a-z0-9]+", "")
end

local function provider_headers(provider)
    local headers = {}
    if provider and provider.api_key and provider.api_key ~= "" then
        headers["Authorization"] = "Bearer " .. provider.api_key
    end
    return headers
end

local function provider_models_url(provider)
    if provider and provider.models_endpoint and provider.models_endpoint ~= "" then
        return provider.models_endpoint
    end
    if provider and provider.endpoint then
        local base = provider.endpoint:gsub("/chat/completions$", "")
        if base ~= provider.endpoint then
            return base .. "/models"
        end
    end
    return nil
end

local function load_provider_models(provider)
    if provider and type(provider.models) == "table" then
        return provider.models
    end
    local url = provider_models_url(provider)
    if not url then return nil end
    if models_cache_by_url[url] ~= nil then
        return models_cache_by_url[url]
    end

    local status, body = http.get(url, provider_headers(provider))
    if status == 200 and body and body ~= "" then
        local ok, decoded = pcall(json.decode, body)
        if ok and decoded and type(decoded.data) == "table" then
            models_cache_by_url[url] = decoded.data
            return decoded.data
        end
    end

    models_cache_by_url[url] = false
    return nil
end

local function provider_model_limit(provider)
    if not provider or not provider.model or provider.model == "" then return 0 end
    local models = load_provider_models(provider)
    if not models then return 0 end

    local wanted = normalized_model_id(provider.model)
    for _, item in ipairs(models) do
        if type(item) == "table" then
            if item.id == provider.model or item.canonical_slug == provider.model or item.name == provider.model then
                return M.model_context_length(item)
            end
            if wanted ~= "" and
               (normalized_model_id(item.id) == wanted or
                normalized_model_id(item.canonical_slug) == wanted or
                normalized_model_id(item.name) == wanted) then
                return M.model_context_length(item)
            end
        end
    end
    return 0
end

function M.ensure_context_limit(provider)
    if provider.context_limit and provider.context_limit > 0 then
        return provider.context_limit
    end
    provider.context_limit = provider_model_limit(provider)
    return provider.context_limit or 0
end

local function model_label(item)
    local id = tostring(item.id or item.canonical_slug or "")
    local name = tostring(item.name or "")
    if name ~= "" and name ~= id then
        return id .. "  " .. name
    end
    return id
end

local function normalize_models_response(decoded)
    local data = decoded and decoded.data
    if type(data) ~= "table" then return {} end

    local models = {}
    for _, item in ipairs(data) do
        if type(item) == "table" and item.id then
            table.insert(models, {
                id = tostring(item.id),
                text = model_label(item),
                context_limit = M.model_context_length(item),
            })
        end
    end
    table.sort(models, function(a, b) return a.id < b.id end)
    return models
end

function M.list(runtime, provider_name)
    provider_name = provider_name or runtime.provider
    local provider = runtime.providers[provider_name]
    if not provider then
        return nil, "Unknown provider: " .. tostring(provider_name)
    end

    if type(provider.models) == "table" then
        return normalize_models_response({data = provider.models}), nil
    end

    local url = provider_models_url(provider)
    if not url then
        return nil, "Provider does not expose a models endpoint: " .. tostring(provider_name)
    end

    local status, body = http.get(url, provider_headers(provider))
    if status < 200 or status >= 300 then
        return nil, string.format("Models request failed: HTTP %d", status)
    end

    local ok, decoded = pcall(json.decode, body or "")
    if not ok then
        return nil, "Models response is not valid JSON"
    end
    return normalize_models_response(decoded), nil
end

function M.list_all(runtime)
    local items = {}
    local provider_names = {}
    for provider_name, _ in pairs(runtime.providers or {}) do
        table.insert(provider_names, provider_name)
    end
    table.sort(provider_names)

    for _, provider_name in ipairs(provider_names) do
        local list = M.list(runtime, provider_name)
        if list then
            for _, model in ipairs(list) do
                table.insert(items, {
                    provider = provider_name,
                    id = model.id,
                    text = string.format("%s/%s", provider_name, model.text or model.id),
                    context_limit = model.context_limit,
                })
            end
        end
    end
    return items
end

function M.set(runtime, provider_name, model)
    provider_name = provider_name or runtime.provider
    if type(model) ~= "string" or model == "" then
        return false, "Missing model"
    end
    local provider = runtime.providers[provider_name]
    if not provider then
        return false, "Unknown provider: " .. tostring(provider_name)
    end
    provider.model = model
    provider.context_limit = 0
    runtime.provider = provider_name
    state.set_model(provider_name, model)
    if agent and agent.set_info then
        agent.set_info(provider_name, provider.model)
    end
    return true, nil
end

function M.set_weak(runtime, provider_name, model)
    if type(model) ~= "string" or model == "" then
        return false, "Missing model"
    end
    if type(provider_name) ~= "string" or provider_name == "" then
        return false, "Missing provider"
    end
    if not runtime.providers[provider_name] then
        return false, "Unknown provider: " .. tostring(provider_name)
    end
    runtime.weak_model = { provider = provider_name, model = model }
    return state.set_weak_model(provider_name, model)
end

function M.weak(runtime)
    local weak = runtime.weak_model
    if type(weak) == "table" and
       type(weak.provider) == "string" and weak.provider ~= "" and
       type(weak.model) == "string" and weak.model ~= "" then
        return { provider = weak.provider, model = weak.model }
    end
    return nil
end

function M.profile(runtime, profile_name)
    local normalized = profiles.normalize(profile_name)
    if not normalized then return nil end
    local profile_models = runtime.profile_models
    local value = type(profile_models) == "table" and profile_models[normalized] or nil
    if type(value) == "table" and
       type(value.provider) == "string" and value.provider ~= "" and
       type(value.model) == "string" and value.model ~= "" then
        return { provider = value.provider, model = value.model }
    end
    return nil
end

function M.set_profile(runtime, profile_name, provider_name, model)
    local normalized = profiles.normalize(profile_name)
    if not normalized then
        return false, "Unknown profile: " .. tostring(profile_name)
    end
    if type(model) ~= "string" or model == "" then
        return false, "Missing model"
    end
    if type(provider_name) ~= "string" or provider_name == "" then
        return false, "Missing provider"
    end
    if not runtime.providers[provider_name] then
        return false, "Unknown provider: " .. tostring(provider_name)
    end
    if type(runtime.profile_models) ~= "table" then
        runtime.profile_models = {}
    end
    runtime.profile_models[normalized] = { provider = provider_name, model = model }
    return state.set_profile_model(normalized, provider_name, model)
end

function M.install_runtime_api(runtime)
    if not _G.capstan then _G.capstan = {} end
    _G.capstan.models = {
        current_provider = function() return runtime.provider end,
        current_model = function()
            local provider = runtime.providers[runtime.provider]
            return provider and provider.model or ""
        end,
        list = function()
            return M.list(runtime, runtime.provider)
        end,
        list_all = function()
            return M.list_all(runtime)
        end,
        set = function(model)
            return M.set(runtime, runtime.provider, model)
        end,
        set_for = function(provider_name, model)
            return M.set(runtime, provider_name, model)
        end,
        weak = function()
            return M.weak(runtime)
        end,
        set_weak = function(provider_name, model)
            return M.set_weak(runtime, provider_name, model)
        end,
        profile = function(profile_name)
            return M.profile(runtime, profile_name)
        end,
        set_profile = function(profile_name, provider_name, model)
            return M.set_profile(runtime, profile_name, provider_name, model)
        end,
    }
end

return M
