-- Authentication is owned by agent.openai_siwc_auth, never legacy API-key storage.
return {
    id = 'openai_siwc',
    name = 'OpenAI Sign in with ChatGPT',
    description = 'Built-in SIWC Responses provider (distinct from openai_codex).',
    auth = {
        provider = 'openai_siwc',
        label = 'Continue with ChatGPT — /connect openai_siwc [account-id]',
        persistence = 'managed',
        default_method = 'browser',
        authorize = function(_, ctx)
            ctx.display_auth = function(label, url)
                if not (popup and type(popup.info) == 'function') then
                    error('ChatGPT sign-in requires the interactive interface', 0)
                end
                popup.info(label, 'Open this URL in your browser to continue:\n\n' .. url)
            end
            return require('agent.openai_siwc_auth').authorize(ctx, ctx.args and ctx.args[2])
        end,
        status = function()
            local accounts, err = require('agent.openai_siwc_auth').status()
            if not accounts then return nil, err end
            local rows = {}
            for _, account in ipairs(accounts) do
                rows[#rows + 1] = {id = account.account_id, email = account.email,
                    connected = account.authenticated}
            end
            return rows
        end,
        logout = function(account_id) return require('agent.openai_siwc_auth').logout(account_id) end,
    },
}
