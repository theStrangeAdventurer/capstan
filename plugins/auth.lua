local plugin = {}

plugin.id = "auth"
plugin.name = "Auth"
plugin.description = "Show OAuth provider status"
plugin.command = "/auth"
plugin.async = false
plugin.history = false

local auth = require("agent.auth")

local function fmt_expiry(ms)
	if type(ms) ~= "number" then return "unknown" end
	local delta = math.floor((ms - (capstan.now_ms and capstan.now_ms() or os.time() * 1000)) / 1000)
	if delta < 0 then return "expired" end
	return tostring(math.floor(delta / 60)) .. "m"
end

function plugin.handler(ctx)
	local all = auth.list()
	local keys = {}
	for provider, cred in pairs(all) do
		if type(provider) == "string" and type(cred) == "table" then
			table.insert(keys, provider)
		end
	end
	table.sort(keys)
	if #keys == 0 then return ctx:replace("No OAuth credentials stored.") end
	local lines = {"OAuth credentials:"}
	for _, provider in ipairs(keys) do
		local cred = auth.redacted(provider) or {}
		table.insert(lines, string.format("- %s: %s expires=%s", provider, tostring(cred.type), fmt_expiry(cred.expires)))
	end
	return ctx:replace(table.concat(lines, "\n"))
end

return plugin
