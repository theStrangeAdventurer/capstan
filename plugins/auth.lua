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
	local lines = {"Credentials:"}
	local credentials = require("agent.credentials")
	for _, item in ipairs(credentials.list()) do
		table.insert(lines, "- " .. item.id .. ": " .. credentials.source(item.id))
	end
	for _, provider in ipairs(keys) do
		local cred = auth.redacted(provider) or {}
		table.insert(lines, string.format("- %s: %s expires=%s", provider, tostring(cred.type), fmt_expiry(cred.expires)))
	end
	local failed = false
	for _, p in pairs(_G.plugins or {}) do
		local descriptor = type(p) == "table" and p.auth
		if type(descriptor) == "table" and descriptor.persistence == "managed"
			and type(descriptor.provider) == "string" and type(descriptor.status) == "function" then
			local rows, err = descriptor.status()
			if type(rows) ~= "table" then
				failed = true
				table.insert(lines, "- " .. descriptor.provider .. ": Status failed: " .. tostring(err))
			elseif #rows == 0 then
				table.insert(lines, "- " .. descriptor.provider .. ": not connected")
			else
				for _, row in ipairs(rows) do
					if type(row) == "table" then
						local fields = {}
						for _, key in ipairs({"id", "email"}) do
							if type(row[key]) == "string" then
								table.insert(fields, key .. "=" .. row[key]:sub(1, 256):gsub("%c", " "))
							end
						end
						if type(row.connected) == "boolean" then
							table.insert(fields, "connected=" .. tostring(row.connected))
						end
						table.insert(lines, "- " .. descriptor.provider .. ": " .. table.concat(fields, " "))
					end
				end
			end
		end
	end
	if #lines == 1 then return ctx:replace("No OAuth credentials stored.") end
	if failed then return table.concat(lines, "\n"), nil, false end
	return ctx:replace(table.concat(lines, "\n"))
end

return plugin
