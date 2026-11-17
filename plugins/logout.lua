local plugin = {}

plugin.id = "logout"
plugin.name = "Logout"
plugin.description = "Remove OAuth credentials"
plugin.command = "/logout"
plugin.async = false
plugin.history = false

local auth = require("agent.auth")

function plugin.handler(ctx)
	local provider = ctx.args and ctx.args[1]
	if not provider or provider == "" then
		return ctx:replace("Usage: /logout <provider>")
	end
	local credentials = require("agent.credentials")
	if credentials.definition(provider) then
		local message, ok = credentials.logout(provider)
		if not ok then return message, nil, false end
		return ctx:replace(message)
	end
	for _, p in pairs(_G.plugins or {}) do
		if type(p) == "table" and type(p.auth) == "table" and p.auth.provider == provider then
			if type(p.auth.logout) == "function" then
				local ok, message = p.auth.logout(ctx.args[2])
				if not ok then return "Logout failed: " .. tostring(message), nil, false end
				return ctx:replace(message or ("Logged out: " .. provider))
			elseif p.auth.persistence == "managed" then
				return "Logout failed: provider cannot log out: " .. provider, nil, false
			end
		end
	end
	local ok, err = auth.remove(provider)
	if not ok then return "Logout failed: " .. tostring(err), nil, false end
	return ctx:replace("Logged out: " .. provider)
end

return plugin
