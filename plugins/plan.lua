local plugin = {}

plugin.id = "profile_plan"
plugin.name = "Plan Profile"
plugin.description = "Switch to read-only planning profile"
plugin.command = "/plan"
plugin.history = false

function plugin.handler(ctx)
	if capstan and capstan.agent and capstan.agent.set_profile then
		capstan.agent.set_profile("plan")
	end
	return ctx:replace("Profile: plan (read-only planning)", "")
end

return plugin
