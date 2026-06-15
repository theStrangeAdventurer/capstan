local plugin = {}

plugin.id = "file"
plugin.name = "File"
plugin.description = "Read file contents"
plugin.command = "/file"
plugin.async = false

plugin.autocomplete = {
  fetch = function(args)
    local dir = args[1] or "."
    dir = dir:gsub("/+$", "")
    if dir == "" then dir = "." end
    local items = {}
    if dir ~= "." and dir ~= "/" then
      local parent = dir:match("^(.*/)") or "."
      parent = parent:gsub("/+$", "")
      if parent == "" then parent = "/" end
      table.insert(items, {text = "../", value = parent .. "/"})
    end
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
			local ls = io.popen('ls -1p "' .. filename .. '" 2>/dev/null')
			if ls then
				local contents = ls:read("*a")
				ls:close()
				if contents ~= "" then
					table.insert(ui_parts, "📁 " .. filename)
					table.insert(llm_parts, "📁 " .. filename .. "\n" .. contents)
				else
					table.insert(ui_parts, "❌ " .. filename .. " (" .. err .. ")")
					table.insert(llm_parts, "❌ Cannot open " .. filename .. ": " .. err)
				end
			else
				table.insert(ui_parts, "❌ " .. filename .. " (" .. err .. ")")
				table.insert(llm_parts, "❌ Cannot open " .. filename .. ": " .. err)
			end
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

plugin.tool = {
	name = "file_read",
	description = "Read the contents of a file at the given path",
	parameters = {
		type = "object",
		properties = {
			path = { type = "string", description = "Path to the file to read" }
		},
		required = { "path" }
	},
	permission = "file_read"
}

return plugin
