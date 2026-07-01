local workspace = require("agent.workspace")

local plugin = {}

plugin.id = "file"
plugin.name = "File"
plugin.description = "Read file contents"
plugin.command = "/file"
plugin.async = false

local function list_dir(path)
	return io.popen("ls -1p -- " .. workspace.shell_quote(path) .. " 2>/dev/null")
end

plugin.autocomplete = {
  fetch = function(args)
    local dir = workspace.resolve_path(args[1] or ".")
    dir = dir:gsub("/+$", "")
    if dir == "" then dir = "." end
    local items = {}
    if dir ~= "." and dir ~= "/" then
      local parent = dir:match("^(.*/)") or "."
      parent = parent:gsub("/+$", "")
      if parent == "" then parent = "/" end
      table.insert(items, {text = "../", value = parent .. "/"})
    end
    local handle = list_dir(dir)
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
	if (#filenames == 0) and ctx.tool_args and type(ctx.tool_args.path) == "string" then
		filenames = { ctx.tool_args.path }
	end
	if #filenames == 0 then
		return "Usage: /file <filename...>"
	end

	local function resolve_filename(filename)
		local resolved = workspace.resolve_path(filename)
		local file, err = io.open(resolved, "r")
		if file then
			return resolved, file, nil
		end

		if filename:match("/?README$") then
			local readme_base = workspace.resolve_path(filename)
			local candidates = {
				readme_base .. ".md",
				readme_base .. ".txt",
				readme_base .. ".markdown",
				readme_base:match("^(.*)/README$") and readme_base:gsub("README$", "readme.md") or workspace.resolve_path("readme.md"),
			}
			for _, candidate in ipairs(candidates) do
				file = io.open(candidate, "r")
				if file then
					return candidate, file, nil
				end
			end
		end

		return filename, nil, err
	end

	local ui_parts = {}
	local llm_parts = {}

	for _i, filename in ipairs(filenames) do
		if ctx.tool_args then
			local allowed, reason = workspace.model_path_allowed(filename, "read", {
				allow_outside_workspace = ctx.permission and ctx.permission.allow_outside_workspace
			})
			if not allowed then
				local resolved = workspace.resolve_path(filename)
				table.insert(ui_parts, "❌ " .. resolved .. " (" .. reason .. ")")
				table.insert(llm_parts, "❌ Cannot open " .. resolved .. ": " .. reason)
				goto continue
			end
		end
		local resolved_filename, file, err = resolve_filename(filename)
		if not file then
			local resolved = workspace.resolve_path(filename)
			local ls = list_dir(resolved)
			if ls then
				local contents = ls:read("*a")
				ls:close()
				if contents ~= "" then
					table.insert(ui_parts, "📁 " .. resolved)
					table.insert(llm_parts, "📁 " .. resolved .. "\n" .. contents)
				else
					table.insert(ui_parts, "❌ " .. resolved .. " (" .. err .. ")")
					table.insert(llm_parts, "❌ Cannot open " .. resolved .. ": " .. err)
				end
			else
				local resolved = workspace.resolve_path(filename)
				table.insert(ui_parts, "❌ " .. resolved .. " (" .. err .. ")")
				table.insert(llm_parts, "❌ Cannot open " .. resolved .. ": " .. err)
			end
		else
			local content = file:read("*a")
			file:close()
			if content == nil then
				local ls = list_dir(resolved_filename)
				local listed = ls and ls:read("*a") or ""
				if ls then ls:close() end
				if listed ~= "" then
					table.insert(ui_parts, "📁 " .. resolved_filename)
					table.insert(llm_parts, "📁 " .. resolved_filename .. "\n" .. listed)
				else
					table.insert(ui_parts, "❌ " .. resolved_filename .. " (could not read file or list directory)")
					table.insert(llm_parts, "❌ Cannot open " .. resolved_filename .. ": could not read file or list directory")
				end
			else
				table.insert(ui_parts, "📄 " .. resolved_filename)
				table.insert(llm_parts, string.format(
					"📄 %s\n─────────────\n%s\n─────────────",
					resolved_filename, content
				))
			end
		end
		::continue::
	end

	local ui_value = table.concat(ui_parts, "\n")
	local llm_value = table.concat(llm_parts, "\n\n")
	return ctx:replace(ui_value, llm_value)
end

plugin.tool = {
	name = "file_read",
	description = "Read a local file or list a local directory. Use this for local file inspection instead of shell commands like cat, sed, or ls.",
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
