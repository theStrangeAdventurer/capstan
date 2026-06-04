local plugin = {}

plugin.id = "greetings"
plugin.name = "Greetings"
plugin.description = "Replace greeting commands with emoji"
plugin.command = "/hi"
plugin.async = false

function plugin.handler(ctx)
	local name = ctx.args[1] or '<Anonymous>'
	return ctx:replace("👋, " .. name .. "!")
end

return plugin
