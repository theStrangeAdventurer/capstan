local plugin = {}

plugin.id = "refresh"
plugin.name = "Refresh"
plugin.description = "Rescan skills, wiki and instruction metadata without restarting"
plugin.command = "/refresh"
plugin.async = false
plugin.history = false

function plugin.handler(ctx)
	if capstan and type(capstan.refresh_context) == "function" then
		local ok, err = pcall(capstan.refresh_context)
		if not ok then
			return ctx:replace("Context refresh failed: " .. tostring(err))
		end
	else
		return ctx:replace("Context refresh is unavailable in this runtime.")
	end
	local parts = {"Skills and wiki context refreshed."}
	if capstan.skills_summary and capstan.skills_summary ~= "" then
		parts[#parts + 1] = capstan.skills_summary
	end
	if capstan.wiki_summary and capstan.wiki_summary ~= "" then
		parts[#parts + 1] = capstan.wiki_summary
	end
	return ctx:replace(table.concat(parts, "\n\n"))
end

return plugin
