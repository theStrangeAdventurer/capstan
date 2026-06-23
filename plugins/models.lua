local plugin = {}

plugin.id = "models"
plugin.name = "Models"
plugin.description = "Select model for current provider"
plugin.command = "/models"
plugin.async = false
plugin.history = false

local function runtime()
	if capstan and capstan.models then
		return capstan.models
	end
	return nil
end

plugin.autocomplete = {
	fetch = function(_args)
		local models = runtime()
		if not models or not models.list then
			return {}
		end
		local list, err = models.list()
		if not list then
			return {
				{ text = err or "Cannot load models", value = "" }
			}
		end
		local items = {}
		for _, model in ipairs(list) do
			table.insert(items, {
				text = model.text or model.id,
				value = model.id,
			})
		end
		return items
	end,
	title = "Models",
	limit = 10,
	multi = false,
}

function plugin.handler(ctx)
	local model = ctx.args[1]
	if not model or model == "" then
		return ctx:replace("Usage: /models <model>")
	end

	local models = runtime()
	if not models or not models.set then
		return ctx:replace("Cannot set model: provider runtime is not initialized")
	end

	local ok, err = models.set(model)
	if not ok then
		return ctx:replace("Cannot set model: " .. tostring(err))
	end

	local provider = models.current_provider and models.current_provider() or "provider"
	return ctx:replace(string.format("Model set: %s/%s", provider, model))
end

return plugin
