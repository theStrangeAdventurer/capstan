local plugin = {}

plugin.id = "connect"
plugin.name = "Connect"
plugin.description = "Connect OAuth providers"
plugin.command = "/connect"
plugin.async = false
plugin.history = false

local auth = require("agent.auth")

local function providers()
	local out = {}
	for _, p in pairs(_G.plugins or {}) do
		if type(p) == "table" and type(p.auth) == "table" and type(p.auth.provider) == "string" then
			table.insert(out, p)
		end
	end
	table.sort(out, function(a, b) return a.auth.provider < b.auth.provider end)
	return out
end

local function find_provider(name)
	for _, p in ipairs(providers()) do
		if p.auth.provider == name then return p end
	end
	return nil
end

function plugin.handler(ctx)
	local provider_name = ctx.args and ctx.args[1]
	if not provider_name or provider_name == "" then
		local lines = {"OAuth providers:"}
		local list = providers()
		if #list == 0 then return ctx:replace("No OAuth providers available.") end
		for _, p in ipairs(list) do
			table.insert(lines, "- " .. p.auth.provider)
		end
		table.insert(lines, "")
		table.insert(lines, "Usage: /connect <provider> [method]")
		return ctx:replace(table.concat(lines, "\n"))
	end

	local p = find_provider(provider_name)
	if not p then return ctx:replace("Unknown OAuth provider: " .. provider_name) end
	if type(p.auth.authorize) ~= "function" then
		return ctx:replace("Provider cannot authorize: " .. provider_name)
	end
	local method = ctx.args and ctx.args[2] or nil
	local credential, err = p.auth.authorize(method or "device", ctx)
	if not credential then
		return ctx:replace("Connect failed: " .. tostring(err))
	end
	local ok, save_err = auth.set(p.auth.provider, credential)
	if not ok then return ctx:replace("Connect failed: " .. tostring(save_err)) end
	return ctx:replace("Connected: " .. p.auth.provider)
end

return plugin
