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

local function packed_value(kind, provider, model)
	return table.concat({ kind, provider or "", model or "" }, "\t")
end

local function unpack_packed(value)
	if type(value) ~= "string" then
		return nil
	end
	local kind, provider, model = value:match("^([^\t]+)\t([^\t]+)\t(.+)$")
	if kind and provider and model then
		return kind, provider, model
	end
	return nil
end

local function selected_kind(args)
	if args and args[1] == "--weak" then
		return "weak"
	end
	return "primary"
end

plugin.autocomplete = {
	fetch = function(args)
		local models = runtime()
		if not models or not models.list then
			return {}
		end
		local kind = selected_kind(args)
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
			table.insert(items, {
				text = string.format("%s  %s", kind == "weak" and "weak" or "main", model.text or model.id),
				value = packed_value(kind, provider, model.id),
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
	local kind, provider, model = unpack_packed(args[1])
	if not kind then
		if args[1] == "--weak" then
			kind = "weak"
			provider = args[2]
			model = args[3]
		elseif args[1] and args[2] then
			kind = "primary"
			provider = args[1]
			model = args[2]
		else
			kind = "primary"
			provider = nil
			model = args[1]
		end
	end

	if not model or model == "" then
		return ctx:replace("Usage: /models [--weak] [provider] <model>")
	end

	local models = runtime()
	if not models then
		return ctx:replace("Cannot set model: provider runtime is not initialized")
	end

	if kind == "weak" then
		provider = provider or (models.current_provider and models.current_provider())
		if not models.set_weak then
			return ctx:replace("Cannot set weak model: provider runtime does not support weak model state")
		end
		local ok, err = models.set_weak(provider, model)
		if not ok then
			return ctx:replace("Cannot set weak model: " .. tostring(err))
		end
		return ctx:replace(string.format("Weak model set: %s/%s", provider, model))
	end

	local ok, err
	if provider and models.set_for then
		ok, err = models.set_for(provider, model)
	else
		provider = models.current_provider and models.current_provider() or "provider"
		ok, err = models.set(model)
	end
	if not ok then
		return ctx:replace("Cannot set model: " .. tostring(err))
	end
	return ctx:replace(string.format("Model set: %s/%s", provider, model))
end

return plugin
