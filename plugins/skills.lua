local plugin = {}

plugin.id = "skills"
plugin.name = "Skills"
plugin.description = "Show loaded skills"
plugin.command = "/skills"
plugin.async = false

function plugin.handler(ctx)
	local summary = "No skills loaded."
	if capstan and capstan.skills_summary and capstan.skills_summary ~= "" then
		summary = capstan.skills_summary
	end
	return ctx:replace(summary)
end

return plugin
