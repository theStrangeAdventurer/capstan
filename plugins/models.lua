local plugin = {}

plugin.id = "models"
plugin.name = "Models"
plugin.description = "Select primary or weak model"
plugin.command = "/models"
plugin.async = false
plugin.history = false

local function runtime()
	if capstan and capstan.models then
		return capstan.models
	end
	return nil
end

local function packed_value(kind, profile, provider, model)
	if kind == "profile" then
		return table.concat({ kind, profile or "", provider or "", model or "" }, "\t")
	end
	return table.concat({ kind, provider or "", model or "" }, "\t")
end

local function effort_drill_value(kind, profile, provider, model, efforts)
	return table.concat({ "efforts", kind, profile or "", provider or "", model or "", table.concat(efforts, ",") }, "\t") .. "/"
end

local function packed_effort_value(kind, profile, provider, model, effort)
	return table.concat({ "choice", kind, profile or "", provider or "", model or "", effort }, "\t")
end

local function unpack_effort_drill(value)
	if type(value) ~= "string" or value:sub(-1) ~= "/" then return nil end
	local kind, profile, provider, model, efforts = value:sub(1, -2):match(
		"^efforts\t([^\t]+)\t([^\t]*)\t([^\t]+)\t([^\t]+)\t(.+)$"
	)
	if not kind then return nil end
	local result = {}
	for effort in efforts:gmatch("[^,]+") do table.insert(result, effort) end
	return kind, profile ~= "" and profile or nil, provider, model, result
end

local function unpack_packed(value)
	if type(value) ~= "string" then
		return nil
	end
	local choice_kind, choice_profile, choice_provider, choice_model, effort = value:match(
		"^choice\t([^\t]+)\t([^\t]*)\t([^\t]+)\t([^\t]+)\t([^\t]+)$"
	)
	if choice_kind then
		return choice_kind, choice_profile ~= "" and choice_profile or nil,
			choice_provider, choice_model, effort
	end
	local profile, provider, model = value:match("^profile\t([^\t]+)\t([^\t]+)\t(.+)$")
	if profile and provider and model then
		return "profile", profile, provider, model
	end
	local kind, provider, model = value:match("^([^\t]+)\t([^\t]+)\t(.+)$")
	if kind and provider and model then
		return kind, nil, provider, model
	end
	return nil
end

local function selected_kind(args)
	if args and args[1] == "--weak" then
		return "weak", nil
	end
	if args and args[1] == "--profile" then
		return "profile", args[2]
	end
	if capstan and capstan.agent and type(capstan.agent.get_profile) == "function" then
		local ok, profile = pcall(capstan.agent.get_profile)
		if ok and type(profile) == "string" and profile ~= "" then
			return "profile", profile
		end
	end
	return "profile", "implement"
end

local function refresh_status()
	if capstan and capstan.agent and capstan.agent.refresh_status then
		capstan.agent.refresh_status()
	end
end

plugin.autocomplete = {
	fetch = function(args)
		local models = runtime()
		if not models or not models.list then
			return {}
		end
		local kind, profile = selected_kind(args)
		local drill_kind, drill_profile, drill_provider, drill_model, efforts =
			unpack_effort_drill(args and args[1])
		if drill_kind then
			local items = {{
				text = "Reasoning · Default",
				value = packed_effort_value(drill_kind, drill_profile, drill_provider, drill_model, "default"),
			}}
			for _, effort in ipairs(efforts) do
				table.insert(items, {
					text = "Reasoning · " .. effort:sub(1, 1):upper() .. effort:sub(2),
					value = packed_effort_value(drill_kind, drill_profile, drill_provider, drill_model, effort),
				})
			end
			return items
		end
		local list, err
		if models.list_all then
			list, err = models.list_all()
		else
			list, err = models.list()
		end
		if not list then
			return {
				{ text = err or "Cannot load models", value = "" }
			}
		end
		local items = {}
		for _, model in ipairs(list) do
			local provider = model.provider
			if not provider and models.current_provider then
				provider = models.current_provider()
			end
			local value = packed_value(kind, profile, provider, model.id)
			if type(model.reasoning_efforts) == "table" and #model.reasoning_efforts > 0 then
				value = effort_drill_value(kind, profile, provider, model.id, model.reasoning_efforts)
			end
			table.insert(items, {
				text = string.format("%s  %s", kind == "weak" and "weak" or kind == "profile" and (profile or "profile") or "main", model.text or model.id),
				value = value,
			})
		end
		return items
	end,
	title = "Models",
	limit = 10,
	multi = false,
}

function plugin.handler(ctx)
	local args = ctx.args or {}
	local models = runtime()
	local kind, profile, provider, model, reasoning_effort = unpack_packed(args[1])
	if not kind then
		if args[1] == "--weak" then
			kind = "weak"
			provider = args[2]
			model = args[3]
			reasoning_effort = args[4]
		elseif args[1] == "--profile" then
			kind = "profile"
			profile = args[2]
			provider = args[3]
			model = args[4]
			reasoning_effort = args[5]
		elseif args[1] and args[2] then
			kind = "primary"
			provider = args[1]
			model = args[2]
			reasoning_effort = args[3]
		else
			kind = "profile"
			_, profile = selected_kind(args)
			provider = models and models.current_provider and models.current_provider() or nil
			model = args[1]
			reasoning_effort = args[2]
		end
	end

	if not model or model == "" then
		return ctx:replace("Usage: /models [--weak] [provider] <model> | /models --profile <fast|plan|implement> <provider> <model>")
	end

	if not models then
		return ctx:replace("Cannot set model: provider runtime is not initialized")
	end

	if kind == "weak" then
		provider = provider or (models.current_provider and models.current_provider())
		if not models.set_weak then
			return ctx:replace("Cannot set weak model: provider runtime does not support weak model state")
		end
		local efforts = models.reasoning_efforts and models.reasoning_efforts(provider, model) or {}
		if efforts and #efforts > 0 and not reasoning_effort then
			return ctx:replace("Reasoning effort is required: default, " .. table.concat(efforts, ", "))
		end
		local ok, err = models.set_weak(provider, model, reasoning_effort)
		if not ok then
			return ctx:replace("Cannot set weak model: " .. tostring(err))
		end
		refresh_status()
		return ctx:replace(string.format("Weak model set: %s/%s (reasoning: %s)",
			provider, model, reasoning_effort or "default"))
	end

	if kind == "profile" then
		if not profile or profile == "" then
			return ctx:replace("Cannot set profile model: missing profile")
		end
		if not provider or provider == "" then
			return ctx:replace("Cannot set profile model: missing provider")
		end
		if not models.set_profile then
			return ctx:replace("Cannot set profile model: provider runtime does not support profile model state")
		end
		local efforts = models.reasoning_efforts and models.reasoning_efforts(provider, model) or {}
		if efforts and #efforts > 0 and not reasoning_effort then
			return ctx:replace("Reasoning effort is required: default, " .. table.concat(efforts, ", "))
		end
		local ok, err = models.set_profile(profile, provider, model, reasoning_effort)
		if not ok then
			return ctx:replace("Cannot set profile model: " .. tostring(err))
		end
		refresh_status()
		return ctx:replace(string.format("Profile model set: %s %s/%s (reasoning: %s)",
			profile, provider, model, reasoning_effort or "default"))
	end

	local ok, err
	if provider and models.set_for then
		local efforts = models.reasoning_efforts and models.reasoning_efforts(provider, model) or {}
		if efforts and #efforts > 0 and not reasoning_effort then
			return ctx:replace("Reasoning effort is required: default, " .. table.concat(efforts, ", "))
		end
		ok, err = models.set_for(provider, model, reasoning_effort)
	else
		provider = models.current_provider and models.current_provider() or "provider"
		local efforts = models.reasoning_efforts and models.reasoning_efforts(provider, model) or {}
		if efforts and #efforts > 0 and not reasoning_effort then
			return ctx:replace("Reasoning effort is required: default, " .. table.concat(efforts, ", "))
		end
		ok, err = models.set(model, reasoning_effort)
	end
	if not ok then
		return ctx:replace("Cannot set model: " .. tostring(err))
	end
	refresh_status()
	return ctx:replace(string.format("Model set: %s/%s (reasoning: %s)",
		provider, model, reasoning_effort or "default"))
end

return plugin
