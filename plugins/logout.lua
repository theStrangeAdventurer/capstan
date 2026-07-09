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
	local ok, err = auth.remove(provider)
	if not ok then return ctx:replace("Logout failed: " .. tostring(err)) end
	return ctx:replace("Logged out: " .. provider)
end

return plugin
