local plugin = {}

plugin.id = "connect"
plugin.name = "Connect"
plugin.description = "Connect API-key or OAuth providers"
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
		local list = require("agent.credentials").list()
		for _, p in ipairs(providers()) do
			table.insert(list, {id = p.auth.provider, label = p.auth.label or p.auth.provider})
		end
		-- Keep provider dispatch extensible; expose only the two supported onboarding flows.
		local picker_labels = {opencode_go = "OpenCode Go", openai_siwc = "ChatGPT / Codex"}
		local visible, labels = {}, {}
		for _, item in ipairs(list) do
			local label = picker_labels[item.id]
			if label then
				visible[#visible + 1] = item
				labels[#labels + 1] = label
			end
		end
		list = visible
		if not (_G.popup and type(popup.choice) == "function") then
			return ctx:replace("Connection picker unavailable; use /connect <provider>.")
		end
		local selected = popup.choice("Connect", "Choose a provider", labels)
		for index, label in ipairs(labels) do
			if label == selected then provider_name = list[index].id; break end
		end
		if not provider_name or provider_name == "" then return ctx:replace("Connection cancelled") end
	end

	local credentials = require("agent.credentials")
	if credentials.definition(provider_name) then
		if ctx.args and #ctx.args > 1 then
			return ctx:replace("Do not pass API keys in command arguments; use /connect <provider>.")
		end
		local message, ok = credentials.connect(provider_name)
		if ok == false then return message, nil, false end
		return ctx:replace(message)
	end
	local p = find_provider(provider_name)
	if not p then return ctx:replace("Unknown provider; use /connect to choose one.") end
	if type(p.auth.authorize) ~= "function" then
		return ctx:replace("Provider cannot authorize: " .. provider_name)
	end
	local method = ctx.args and ctx.args[2] or nil
	local credential, err = p.auth.authorize(
		(p.auth.persistence == "managed" and p.auth.default_method) or method or p.auth.default_method or "device", ctx)
	if not credential then
		return "Connect failed: " .. tostring(err), nil, false
	end
	if p.auth.persistence ~= "managed" then
		local ok, save_err = auth.set(p.auth.provider, credential)
		if not ok then return "Connect failed: " .. tostring(save_err), nil, false end
	end
	return ctx:replace("Connected: " .. p.auth.provider)
end

return plugin
