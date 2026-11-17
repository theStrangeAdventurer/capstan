local json = require("vendor.rxi.json")
local profiles = require("agent.profiles")
local state = require("agent.state")

local M = {}

local models_cache_by_url = {}
local reasoning_effort_order = {"none", "minimal", "low", "medium", "high", "xhigh", "max"}
local valid_reasoning_efforts = {}
for _, effort in ipairs(reasoning_effort_order) do valid_reasoning_efforts[effort] = true end

local function normalize_selected_reasoning_effort(value)
    if value == nil or value == "" then return nil end
    if type(value) ~= "string" then return nil, "Invalid reasoning effort" end
    value = value:lower()
    if value == "default" or valid_reasoning_efforts[value] then return value end
    return nil, "Unknown reasoning effort: " .. value
end

function M.model_context_length(item)
    if type(item.context_limit) == "number" and item.context_limit > 0 then
        return item.context_limit
    end
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

local function provider_headers(provider, url)
    local headers = {}
    local key = require("agent.credentials").resolve(nil, provider, url)
    if key then headers["Authorization"] = "Bearer " .. key end
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

-- Go has a public, plan-specific catalog. Never send stored credentials to it,
-- and never use catalog API URLs to change the selected inference endpoint.
local go_catalog_time = {}
local function go_catalog(provider)
    local url = provider_models_url(provider)
    if not url then return nil, 'OpenCode Go models endpoint is missing' end
    local now = os.time()
    if models_cache_by_url[url] and now - (go_catalog_time[url] or 0) < 300 then
        return models_cache_by_url[url]
    end
    local ok, status, body = pcall(http.get, url,
        {['User-Agent'] = 'capstan/' .. tostring((_G.capstan and capstan.version) or 'local')},
        {https_only = true, follow_redirects = false})
    local function unavailable(reason)
        local message = 'OpenCode Go catalog: ' .. reason
        if models_cache_by_url[url] then return models_cache_by_url[url], message .. '; showing cached list' end
        return nil, message
    end
    if not ok or status ~= 200 then
        return unavailable(type(status) == 'number' and ('HTTP ' .. tostring(status)) or 'request failed')
    end
    local decoded_ok, decoded = pcall(json.decode, body or '')
    if not decoded_ok or type(decoded) ~= 'table' or type(decoded.data) ~= 'table' then
        return unavailable('invalid response')
    end
    local result, seen = {}, {}
    for _, item in ipairs(decoded.data) do
        if type(item) == 'table' and type(item.go) == 'table' and item.type == 'chat' and
            type(item.id) == 'string' and item.id ~= '' and not item.id:find('%c') and not seen[item.id] then
            local text_output = false
            local output = type(item.modalities) == 'table' and item.modalities.output
            for _, modality in ipairs(type(output) == 'table' and output or {}) do
                if modality == 'text' then text_output = true end
            end
            if text_output then
                local limit = type(item.limit) == 'table' and item.limit.context
                if type(limit) ~= 'number' or limit ~= limit or limit <= 0 or limit >= math.huge or limit % 1 ~= 0 then limit = 0 end
                local reasoning = type(item.capabilities) == 'table' and item.capabilities.reasoning
                local efforts = {}
                if type(reasoning) == 'table' and reasoning.supported == true and type(reasoning.effort) == 'table' then
                    local unique = {}
                    for _, effort in ipairs(reasoning.effort) do
                        if type(effort) == 'string' and valid_reasoning_efforts[effort] and not unique[effort] then
                            efforts[#efforts + 1] = effort; unique[effort] = true
                        end
                    end
                end
                result[#result + 1] = {id = item.id,
                    name = type(item.name) == 'string' and item.name:gsub('%c', ' ') or item.id,
                    context_limit = limit, reasoning_efforts = efforts}
                seen[item.id] = true
            end
        end
    end
    if #result == 0 then return unavailable('no text models in response') end
    models_cache_by_url[url], go_catalog_time[url] = result, now
    return result
end

local function load_provider_models(provider)
    if provider and type(provider.models) == "table" then
        return provider.models
    end
    if provider and provider.catalog_format == 'opencode_go' then return go_catalog(provider) end
    local url = provider_models_url(provider)
    if not url then return nil end
    if models_cache_by_url[url] ~= nil then
        return models_cache_by_url[url]
    end

    local status, body = http.get(url, provider_headers(provider, url))
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

local function provider_model_limit(provider, catalog)
    if not provider or not provider.model or provider.model == "" then return 0 end
    local models = catalog or load_provider_models(provider)
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
    local configured = provider_model_limit(provider, provider.models or {})
    if configured <= 0 then configured = provider.configured_context_limit or 0 end
    if configured > 0 then
        provider.context_limit = configured
        return configured
    end
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

local function copy_reasoning_efforts(value)
    if type(value) ~= "table" then return nil end
    local result = {}
    for _, effort in ipairs(value) do
        effort = type(effort) == "string" and effort:lower() or nil
        if effort and valid_reasoning_efforts[effort] then table.insert(result, effort) end
    end
    return #result > 0 and result or nil
end

local function item_supports_reasoning_effort(item)
    if item.supported_parameters == nil then return nil end
    if type(item.supported_parameters) ~= "table" then return false end
    for _, parameter in ipairs(item.supported_parameters) do
        if parameter == "reasoning" or parameter == "reasoning_effort" then return true end
    end
    return false
end

local function model_reasoning_efforts(provider, item)
    local configured = provider and provider.reasoning_efforts
    local id = tostring(item.id or item.canonical_slug or "")
    if type(configured) == "table" then
        local per_model = copy_reasoning_efforts(configured[id])
        if per_model then return per_model end
    end
    if item_supports_reasoning_effort(item) == false then return nil end
    local fallback = copy_reasoning_efforts(provider and provider.default_reasoning_efforts)
    if fallback then return fallback end
    if provider and provider.catalog_format == 'opencode_go' then
        return copy_reasoning_efforts(item.reasoning_efforts)
    end
end

local function normalize_models_response(decoded, provider)
    local data = decoded and decoded.data
    if type(data) ~= "table" then return {} end

    local models = {}
    for _, item in ipairs(data) do
        if type(item) == "table" and item.id then
            table.insert(models, {
                id = tostring(item.id),
                text = model_label(item),
                context_limit = M.model_context_length(item),
                reasoning_efforts = model_reasoning_efforts(provider, item),
            })
        end
    end
    table.sort(models, function(a, b) return a.id < b.id end)
    return models
end

local siwc_cache = {}
function M.siwc_list(account_id)
    local credential, err = require('agent.provider_config').siwc_access(account_id)
    if not credential then return nil, err end
    local key = credential.account_id .. ':' .. tostring(credential.generation)
    if siwc_cache[key] then return siwc_cache[key], nil, credential.account_id end
    local ok, status, body = pcall(http.get, 'https://api.openai.com/v1/models',
        {Authorization = 'Bearer ' .. credential.access}, {https_only = true, follow_redirects = false})
    if not ok or status ~= 200 then return nil, 'SIWC model catalog request failed' end
    local decoded_ok, decoded = pcall(json.decode, body or '')
    if not decoded_ok or type(decoded) ~= 'table' or type(decoded.models) ~= 'table' then
        return nil, 'SIWC model catalog must contain models[]'
    end
    local result = {}
    for _, item in ipairs(decoded.models) do
        if type(item) == 'table' and item.visibility == 'list' and
            type(item.slug) == 'string' and item.slug ~= '' then
            result[#result + 1] = {id = item.slug,
                text = type(item.display_name) == 'string' and item.display_name or item.slug,
                context_limit = 0}
        end
    end
    if #result == 0 then return nil, 'SIWC account has no visible models' end
    siwc_cache[key] = result
    return result, nil, credential.account_id
end

function M.list(runtime, provider_name)
    provider_name = provider_name or runtime.provider
    if provider_name == "openai_siwc" then
        local catalog, err, account = M.siwc_list()
        if not catalog then return nil, err end
        local provider = runtime.providers[provider_name]
        local result = {}
        for _, item in ipairs(catalog) do
            result[#result + 1] = {id = item.id, text = item.text,
                context_limit = provider_model_limit({model = item.id}, provider.models or {}),
                reasoning_efforts = model_reasoning_efforts(provider, item)}
        end
        return result, nil, account
    end
    local provider = runtime.providers[provider_name]
    if not provider then
        return nil, "Unknown provider: " .. tostring(provider_name)
    end

    if type(provider.models) == "table" then
        return normalize_models_response({data = provider.models}, provider), nil
    end
    if provider.catalog_format == 'opencode_go' then
        local data, err = go_catalog(provider)
        if not data then return nil, err end
        return normalize_models_response({data = data}, provider), err
    end

    local url = provider_models_url(provider)
    if not url then
        return nil, "Provider does not expose a models endpoint: " .. tostring(provider_name)
    end

    local status, body = http.get(url, provider_headers(provider, url))
    if status < 200 or status >= 300 then
        return nil, string.format("Models request failed: HTTP %d", status)
    end

    local ok, decoded = pcall(json.decode, body or "")
    if not ok then
        return nil, "Models response is not valid JSON"
    end
    models_cache_by_url[url] = type(decoded) == "table" and decoded.data or nil
    return normalize_models_response(decoded, provider), nil
end

-- Hotkeys must never fetch a catalog or enter a blocking HTTP wait.
function M.cached_reasoning_efforts(runtime, provider_name, model)
    local provider = runtime.providers[provider_name]
    if not provider then return {} end
    local catalog = provider.models
    if type(catalog) ~= "table" then
        local url = provider_models_url(provider)
        catalog = url and models_cache_by_url[url]
    end
    for _, item in ipairs(type(catalog) == "table" and catalog or {}) do
        if type(item) == "table" and item.id == model then
            return model_reasoning_efforts(provider, item) or {}
        end
    end
    return model_reasoning_efforts(provider, {id = model}) or {}
end

function M.step_reasoning_effort(efforts, current, direction)
    if direction ~= 1 and direction ~= -1 then return nil, "Invalid effort direction" end
    local supported = {}
    for _, effort in ipairs(efforts or {}) do supported[effort] = true end
    local choices = {"default"}
    for _, effort in ipairs(reasoning_effort_order) do
        if supported[effort] then table.insert(choices, effort) end
    end
    if #choices == 1 then
        return nil, "Reasoning levels unavailable; select a model with /models"
    end
    current = current or "default"
    local rank = {default = 0}
    for i, effort in ipairs(reasoning_effort_order) do rank[effort] = i end
    local current_rank = rank[current] or 0
    if direction > 0 then
        for _, effort in ipairs(choices) do
            if rank[effort] > current_rank then return effort end
        end
    else
        for i = #choices, 1, -1 do
            if rank[choices[i]] < current_rank then return choices[i] end
        end
    end
    return current
end

function M.list_all(runtime)
    local items, warnings = {}, {}
    local provider_names = {}
    for provider_name, _ in pairs(runtime.providers or {}) do
        table.insert(provider_names, provider_name)
    end
    table.sort(provider_names)

    for _, provider_name in ipairs(provider_names) do
        local provider = runtime.providers[provider_name]
        local list, err = M.list(runtime, provider_name)
        if err and provider.catalog_format == 'opencode_go' then warnings[#warnings + 1] = err end
        list = list or {}
        local seen = {}
        for _, model in ipairs(list) do
            seen[model.id] = true
            table.insert(items, {
                provider = provider_name,
                id = model.id,
                text = string.format("%s/%s", provider_name, model.text or model.id),
                context_limit = model.context_limit,
                reasoning_efforts = model.reasoning_efforts,
            })
        end

        local configured_model = provider and provider.model
        if provider_name ~= "openai_siwc" and provider.catalog_format ~= 'opencode_go' and
            type(configured_model) == "string" and configured_model ~= "" and not seen[configured_model] then
            table.insert(items, {
                provider = provider_name,
                id = configured_model,
                text = string.format("%s/%s", provider_name, configured_model),
                context_limit = provider.context_limit or 0,
                reasoning_efforts = model_reasoning_efforts(provider, {id = configured_model}),
            })
        end
    end
    return items, #warnings > 0 and table.concat(warnings, '; ') or nil
end

function M.reasoning_efforts(runtime, provider_name, model)
    local list, err = M.list(runtime, provider_name)
    if not list then return nil, err end
    for _, item in ipairs(list) do
        if item.id == model then return item.reasoning_efforts or {}, nil end
    end
    return {}, nil
end

-- SIWC documents identity/visibility only; capabilities come from existing manual config.
function M.validate_siwc_effort(provider, model, effort)
    if effort == nil or effort == "default" then return true end
    for _, supported in ipairs(model_reasoning_efforts(provider, {id = model}) or {}) do
        if effort == supported then return true end
    end
    return false, "Unsupported SIWC reasoning effort for selected model; configure reasoning_efforts or select default"
end

function M.set(runtime, provider_name, model, reasoning_effort)
    provider_name = provider_name or runtime.provider
    if type(model) ~= "string" or model == "" then
        return false, "Missing model"
    end
    local provider = runtime.providers[provider_name]
    if not provider then
        return false, "Unknown provider: " .. tostring(provider_name)
    end
    if provider_name == "openai_siwc" then
        local valid, effort_error = M.validate_siwc_effort(runtime.providers[provider_name], model, reasoning_effort)
        if not valid then return false, effort_error end
        local catalog, err = M.siwc_list()
        if not catalog then return false, err end
        local visible = false
        for _, item in ipairs(catalog) do if item.id == model then visible = true end end
        if not visible then return false, "SIWC model is not visible; select a model with /models" end
    end
    local normalized_effort, effort_err = normalize_selected_reasoning_effort(reasoning_effort)
    if effort_err then return false, effort_err end
    provider.model = model
    provider.context_limit = 0
    provider.selected_reasoning_effort = normalized_effort
    runtime.provider = provider_name
    state.set_model(provider_name, model, normalized_effort)
    if type(runtime.refresh_status) == "function" then
        runtime.refresh_status()
    elseif agent and agent.set_info then
        agent.set_info(provider_name, provider.model, normalized_effort or "default")
    end
    return true, nil
end

-- Canonical restore policy for commands, shortcuts, status, and requests.
-- Explicit choices are stored verbatim; restored choices must still be supported.
function M.saved_profile_effort(runtime, profile_name, provider_name, model)
    local normalized = profiles.normalize(profile_name)
    if not normalized then return nil end
    local saved = state.profile_reasoning_effort(normalized, provider_name, model)
    if not saved then return nil end
    if saved == "default" then return saved end
    for _, effort in ipairs(M.cached_reasoning_efforts(runtime, provider_name, model)) do
        if effort == saved then return saved end
    end
    return "default"
end

function M.profile(runtime, profile_name)
    local normalized = profiles.normalize(profile_name)
    if not normalized then return nil end
    local profile_models = runtime.profile_models
    local value = type(profile_models) == "table" and profile_models[normalized] or nil
    if type(value) == "table" and
       type(value.provider) == "string" and value.provider ~= "" and
       type(value.model) == "string" and value.model ~= "" then
        return {
            provider = value.provider,
            model = value.model,
            reasoning_effort = M.saved_profile_effort(runtime, normalized, value.provider, value.model)
                or value.reasoning_effort,
        }
    end
    return nil
end

function M.set_profile(runtime, profile_name, provider_name, model, reasoning_effort)
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
    if provider_name == "openai_siwc" then
        local valid, effort_error = M.validate_siwc_effort(runtime.providers[provider_name], model, reasoning_effort)
        if not valid then return false, effort_error end
        local catalog, err = M.siwc_list()
        if not catalog then return false, err end
        local visible = false
        for _, item in ipairs(catalog) do if item.id == model then visible = true end end
        if not visible then return false, "SIWC model is not visible; select a model with /models" end
    end
    local normalized_effort, effort_err = normalize_selected_reasoning_effort(reasoning_effort)
    if effort_err then return false, effort_err end
    if not normalized_effort then
        normalized_effort = M.saved_profile_effort(runtime, normalized, provider_name, model) or "default"
    end
    if type(runtime.profile_models) ~= "table" then
        runtime.profile_models = {}
    end
    runtime.profile_models[normalized] = {
        provider = provider_name,
        model = model,
        reasoning_effort = normalized_effort,
    }
    return state.set_profile_model(normalized, provider_name, model, normalized_effort)
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
        configured = function()
            local items = {}
            for provider_name, provider in pairs(runtime.providers or {}) do
                if type(provider.model) == "string" and provider.model ~= "" then
                    table.insert(items, {provider = provider_name, id = provider.model})
                end
            end
            table.sort(items, function(a, b)
                if a.provider == b.provider then return a.id < b.id end
                return a.provider < b.provider
            end)
            return items
        end,
        reasoning_efforts = function(provider_name, model)
            return M.reasoning_efforts(runtime, provider_name, model)
        end,
        set = function(model, reasoning_effort)
            return M.set(runtime, runtime.provider, model, reasoning_effort)
        end,
        set_for = function(provider_name, model, reasoning_effort)
            return M.set(runtime, provider_name, model, reasoning_effort)
        end,
        saved_profile_effort = function(profile_name, provider_name, model)
            return M.saved_profile_effort(runtime, profile_name, provider_name, model)
        end,
        profile = function(profile_name)
            return M.profile(runtime, profile_name)
        end,
        set_profile = function(profile_name, provider_name, model, reasoning_effort)
            return M.set_profile(runtime, profile_name, provider_name, model, reasoning_effort)
        end,
    }
end

return M
