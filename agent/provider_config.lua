local state = require("agent.state")
local profiles = require("agent.profiles")

local M = {}

local function env_context_limit(...)
    for i = 1, select("#", ...) do
        local value = os.getenv(select(i, ...))
        if value and value ~= "" then
            local n = tonumber(value)
            if n and n > 0 then return n end
        end
    end
    return 0
end

function M.build()
    local env_provider = os.getenv("CAPSTAN_PROVIDER")
    if env_provider == "" then env_provider = nil end
    local env_model = os.getenv("CAPSTAN_MODEL")
    if env_model == "" then env_model = nil end
    local env_context_limit_value = env_context_limit("CAPSTAN_CONTEXT_LIMIT")
    local config = (_G.capstan and _G.capstan.config) or {}
    local runtime = {
        provider = env_provider or state.provider() or config.provider or "deepseek",
        env_provider_override = env_provider ~= nil and env_provider ~= "",
        env_model_overrides = {},
        providers = {
            deepseek = {
                api_key = os.getenv("DEEPSEEK_API_KEY"),
                endpoint = "https://api.deepseek.com/v1/chat/completions",
                model = "deepseek-chat",
                reasoning_effort_field = "reasoning_effort",
                reasoning_history_field = "reasoning_content",
                reasoning_efforts = {
                    ["deepseek-v4-flash"] = {"minimal", "low", "medium", "high", "max"},
                    ["deepseek-v4-pro"] = {"minimal", "low", "medium", "high", "max"},
                },
                context_limit = 0,
            },
            openrouter = {
                api_key = os.getenv("OPENROUTER_API_KEY"),
                endpoint = "https://openrouter.ai/api/v1/chat/completions",
                model = "minimax/minimax-m3",
                default_reasoning_efforts = {"low", "medium", "high"},
                context_limit = 0,
            },
        },
    }
    if type(config.providers) == "table" then
        for name, overrides in pairs(config.providers) do
            if type(name) == "string" and type(overrides) == "table" then
                runtime.providers[name] = runtime.providers[name] or {}
                for key, value in pairs(overrides) do
                    runtime.providers[name][key] = value
                end
            end
        end
    end

    if env_model and env_model ~= "" and runtime.providers[runtime.provider] then
        runtime.env_model_overrides[runtime.provider] = true
    end

    runtime.profile_models = {}
    local agent_config = type(config.agent) == "table" and config.agent or nil
    local configured_profile_models = agent_config and agent_config.profile_models
    if type(configured_profile_models) == "table" then
        for profile_name, value in pairs(configured_profile_models) do
            local normalized = profiles.normalize(profile_name)
            if normalized and type(value) == "table" and
               type(value.provider) == "string" and value.provider ~= "" and
               type(value.model) == "string" and value.model ~= "" then
                runtime.profile_models[normalized] = {
                    provider = value.provider,
                    model = value.model,
                    reasoning_effort = type(value.reasoning_effort) == "string" and
                        value.reasoning_effort or nil,
                }
            end
        end
    end

    local saved_profile_models = state.profile_models()
    for profile_name, value in pairs(saved_profile_models) do
        local normalized = profiles.normalize(profile_name)
        if normalized then
            runtime.profile_models[normalized] = value
        end
    end

    for name, provider in pairs(runtime.providers) do
        local saved_model = state.model_for(name)
        if saved_model then
            provider.model = saved_model
        end
        if not runtime.env_model_overrides[name] then
            provider.selected_reasoning_effort = state.model_reasoning_effort(name)
        end
    end

    local configured_weak = config.weak_model
    if type(configured_weak) == "table" and
       type(configured_weak.provider) == "string" and configured_weak.provider ~= "" and
       type(configured_weak.model) == "string" and configured_weak.model ~= "" then
        runtime.weak_model = {
            provider = configured_weak.provider,
            model = configured_weak.model,
            reasoning_effort = type(configured_weak.reasoning_effort) == "string" and
                configured_weak.reasoning_effort or nil,
        }
    end

    local saved_weak = state.weak_model()
    if saved_weak then
        runtime.weak_model = saved_weak
    end

    if os.getenv("DEEPSEEK_API_KEY") and runtime.providers.deepseek then
        runtime.providers.deepseek.api_key = os.getenv("DEEPSEEK_API_KEY")
    end
    if os.getenv("OPENROUTER_API_KEY") and runtime.providers.openrouter then
        runtime.providers.openrouter.api_key = os.getenv("OPENROUTER_API_KEY")
    end
    local active_provider = runtime.providers[runtime.provider]
    if active_provider and env_model and env_model ~= "" then
        active_provider.model = env_model
    end
    if active_provider and env_context_limit_value > 0 then
        active_provider.context_limit = env_context_limit_value
    end

    return runtime
end

return M
