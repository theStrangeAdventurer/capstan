local state = require("agent.state")

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
    local config = (_G.capstan and _G.capstan.config) or {}
    local runtime = {
        provider = os.getenv("AI_PROVIDER") or config.provider or "deepseek",
        providers = {
            deepseek = {
                api_key = os.getenv("DEEPSEEK_API_KEY"),
                endpoint = "https://api.deepseek.com/v1/chat/completions",
                model = "deepseek-chat",
                context_limit = env_context_limit("DEEPSEEK_CONTEXT_LIMIT", "AI_CONTEXT_LIMIT"),
            },
            openrouter = {
                api_key = os.getenv("OPENROUTER_API_KEY"),
                endpoint = "https://openrouter.ai/api/v1/chat/completions",
                model = os.getenv("OPENROUTER_MODEL") or "minimax/minimax-m3",
                context_limit = env_context_limit("OPENROUTER_CONTEXT_LIMIT", "AI_CONTEXT_LIMIT"),
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

    for name, provider in pairs(runtime.providers) do
        local saved_model = state.model_for(name)
        if saved_model then
            provider.model = saved_model
        end
    end

    if os.getenv("DEEPSEEK_API_KEY") and runtime.providers.deepseek then
        runtime.providers.deepseek.api_key = os.getenv("DEEPSEEK_API_KEY")
    end
    if os.getenv("OPENROUTER_API_KEY") and runtime.providers.openrouter then
        runtime.providers.openrouter.api_key = os.getenv("OPENROUTER_API_KEY")
    end
    if os.getenv("OPENROUTER_MODEL") and runtime.providers.openrouter then
        runtime.providers.openrouter.model = os.getenv("OPENROUTER_MODEL")
    end

    local deepseek_context_limit = env_context_limit("DEEPSEEK_CONTEXT_LIMIT", "AI_CONTEXT_LIMIT")
    if deepseek_context_limit > 0 and runtime.providers.deepseek then
        runtime.providers.deepseek.context_limit = deepseek_context_limit
    end
    local openrouter_context_limit = env_context_limit("OPENROUTER_CONTEXT_LIMIT", "AI_CONTEXT_LIMIT")
    if openrouter_context_limit > 0 and runtime.providers.openrouter then
        runtime.providers.openrouter.context_limit = openrouter_context_limit
    end

    return runtime
end

return M
