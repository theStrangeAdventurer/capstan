local plugin = {}

plugin.id = "info"
plugin.name = "Info"
plugin.description = "Show runtime paths and active model details"
plugin.command = "/info"
plugin.async = false
plugin.history = false

local function call_string(fn, ...)
	if type(fn) ~= "function" then
		return "(unavailable)"
	end
	local ok, value = pcall(fn, ...)
	if not ok or value == nil or value == "" then
		return "(unavailable)"
	end
	return tostring(value)
end

local function current_provider()
	if capstan and capstan.models and capstan.models.current_provider then
		return call_string(capstan.models.current_provider)
	end
	return "(unavailable)"
end

local function current_model()
	if capstan and capstan.models and capstan.models.current_model then
		return call_string(capstan.models.current_model)
	end
	return "(unavailable)"
end

local function weak_model()
	if capstan and capstan.models and capstan.models.weak then
		local ok, value = pcall(capstan.models.weak)
		if ok and type(value) == "table" and value.provider and value.model then
			return tostring(value.provider) .. "/" .. tostring(value.model)
		end
	end
	return "(unavailable)"
end

local function join_path(base, child)
	if not base or base == "" or base == "(unavailable)" then
		return "(unavailable)"
	end
	return tostring(base) .. "/" .. child
end

function plugin.handler(ctx)
	local workdir = tostring((capstan and capstan.workdir) or "(unavailable)")
	local home = os.getenv("HOME")
	local lines = {
		"Capstan runtime",
		"",
		"Workspace",
		"  workdir: " .. workdir,
		"",
		"Configuration",
		"  config dir: " .. call_string(capstan and capstan.config_dir),
		"  config file: " .. call_string(capstan and capstan.config_path, "config.lua"),
		"  plugins dir: " .. call_string(capstan and capstan.config_path, "plugins"),
		"",
		"Skills",
		"  project skills: " .. join_path(workdir, ".agents/skills"),
		"  user config skills: " .. call_string(capstan and capstan.config_path, "skills"),
		"  home skills: " .. (home and join_path(home, ".agents/skills") or "(unavailable)"),
		"  builtin skills: embedded:skills",
		"",
		"State",
		"  state dir: " .. call_string(capstan and capstan.state_dir),
		"  runtime state: " .. call_string(capstan and capstan.state_path, "state.lua"),
		"  permissions: " .. call_string(capstan and capstan.state_path, "permissions.lua"),
		"  current log: " .. call_string(capstan and capstan.log_path),
		"",
		"Agent",
		"  provider: " .. current_provider(),
		"  model: " .. current_model(),
		"  weak model: " .. weak_model(),
	}

	return ctx:replace(table.concat(lines, "\n"))
end

return plugin
