local plugin = {}

plugin.id = "profile_fast"
plugin.name = "Fast Profile"
plugin.description = "Switch to fast low-overhead profile"
plugin.command = "/fast"
plugin.history = false

function plugin.handler(ctx)
	if capstan and capstan.agent and capstan.agent.set_profile then
		capstan.agent.set_profile("fast")
	end
	return ctx:replace("Profile: fast", "")
end

return plugin
