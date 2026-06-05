local plugin = {}

plugin.id = "file"
plugin.name = "File"
plugin.description = "Read file contents"
plugin.command = "/file"
plugin.async = false

plugin.autocomplete = {
  fetch = function(args)
    local dir = args[1] or "."
    local items = {}
    local handle = io.popen('ls -1p "' .. dir .. '" 2>/dev/null')
    if handle then
      for line in handle:lines() do
        if line ~= "" then
          local full = dir .. "/" .. line
          table.insert(items, {text = line, value = full})
        end
      end
      handle:close()
    end
    return items
  end,
  title = "Files",
  limit = 10,
  multi = true,
}

function plugin.handler(ctx)
	local filenames = ctx.args
	if #filenames == 0 then
		return "Usage: /file <filename...>"
	end

	local ui_parts = {}
	local llm_parts = {}

	for _i, filename in ipairs(filenames) do
		local file, err = io.open(filename, "r")
		if not file then
			table.insert(ui_parts, "❌ " .. filename .. " (" .. err .. ")")
			table.insert(llm_parts, "❌ Cannot open " .. filename .. ": " .. err)
		else
			local content = file:read("*a")
			file:close()
			table.insert(ui_parts, "📄 " .. filename)
			table.insert(llm_parts, string.format(
				"📄 %s\n─────────────\n%s\n─────────────",
				filename, content
			))
		end
	end

	local ui_value = table.concat(ui_parts, "\n")
	local llm_value = table.concat(llm_parts, "\n\n")
	return ctx:replace(ui_value, llm_value)
end

return plugin
