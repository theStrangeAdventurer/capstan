local plugin = {}

plugin.id = "profile_implement"
plugin.name = "Implement Profile"
plugin.description = "Switch to focused implementation profile"
plugin.command = "/implement"
plugin.history = false

function plugin.handler(ctx)
	if capstan and capstan.agent and capstan.agent.set_profile then
		capstan.agent.set_profile("implement")
	end
	return ctx:replace("Profile: implement", "")
end

return plugin
