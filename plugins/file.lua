local workspace = require("agent.workspace")
local images_runtime = require("agent.images")

local plugin = {}

plugin.id = "file"
plugin.name = "File"
plugin.description = "Read file contents"
plugin.command = "/file"
plugin.async = false

local function list_dir(path)
	return io.popen("ls -1p -- " .. workspace.shell_quote(path) .. " 2>/dev/null")
end

local function embedded_asset_name(path)
	if type(path) ~= "string" then return nil end
	return path:match("^embedded:(.+)$")
end

local function read_embedded_asset(filename)
	local asset_path = embedded_asset_name(filename)
	if not asset_path then return nil end
	if not (capstan and type(capstan.embedded_asset) == "function") then
		return "❌ " .. filename .. " (embedded assets are unavailable)",
			"❌ Cannot open " .. filename .. ": embedded assets are unavailable"
	end
	local content, err = capstan.embedded_asset(asset_path)
	if type(content) ~= "string" then
		local message = tostring(err or "missing embedded asset")
		return "❌ " .. filename .. " (" .. message .. ")",
			"❌ Cannot open " .. filename .. ": " .. message
	end
	return "📄 " .. filename,
		string.format("📄 %s\n─────────────\n%s\n─────────────", filename, content)
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

local function append_filename(filenames, seen, value)
	if type(value) ~= "string" or value == "" or seen[value] then return end
	seen[value] = true
	table.insert(filenames, value)
end

local function requested_filenames(ctx)
	if type(ctx.tool_args) ~= "table" then
		return ctx.args or {}, false
	end

	local filenames = {}
	local seen = {}
	local has_batch = type(ctx.tool_args.paths) == "table"
	if has_batch then
		for _, path in ipairs(ctx.tool_args.paths) do
			append_filename(filenames, seen, path)
		end
	end
	append_filename(filenames, seen, ctx.tool_args.path)
	return filenames, has_batch
end

function plugin.handler(ctx)
	local filenames, has_batch = requested_filenames(ctx)
	if #filenames == 0 then
		return "Usage: /file <filename...>"
	end

	local function resolve_filename(filename)
		local resolved = workspace.resolve_path(filename)
		local file, err = io.open(resolved, "rb")
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
				file = io.open(candidate, "rb")
				if file then
					return candidate, file, nil
				end
			end
		end

		return filename, nil, err
	end

	local ui_parts = {}
	local llm_parts = {}
	local result_images = {}

	for _i, filename in ipairs(filenames) do
		local embedded_ui, embedded_llm = read_embedded_asset(filename)
		if embedded_ui then
			table.insert(ui_parts, embedded_ui)
			table.insert(llm_parts, embedded_llm)
			goto continue
		end
		if ctx.tool_args then
			if has_batch and workspace.is_sensitive_path(filename) then
				local resolved = workspace.resolve_path(filename)
				table.insert(ui_parts, "❌ " .. resolved .. " (batch reads cannot include sensitive paths)")
				table.insert(llm_parts, "❌ Cannot open " .. resolved .. ": batch reads cannot include sensitive paths; use path for this file")
				goto continue
			end
			local allowed, reason = workspace.model_path_allowed(filename, "read", {
				allow_outside_workspace = not has_batch and ctx.permission and ctx.permission.allow_outside_workspace
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
				local image, image_err = images_runtime.from_bytes(content)
				if image then
					table.insert(ui_parts, string.format("🖼 %s (%s, %d bytes)",
						resolved_filename, image.mime_type, image.bytes))
					table.insert(llm_parts, string.format("🖼 %s (%s, %d bytes; attached for visual inspection)",
						resolved_filename, image.mime_type, image.bytes))
					table.insert(result_images, image)
				elseif image_err == "too_large" then
					table.insert(ui_parts, "❌ " .. resolved_filename .. " (image exceeds 10 MiB limit)")
					table.insert(llm_parts, "❌ Cannot attach " .. resolved_filename .. ": image exceeds 10 MiB limit")
				elseif not images_runtime.is_text(content) then
					table.insert(ui_parts, string.format("📦 %s (%d-byte binary file)", resolved_filename, #content))
					table.insert(llm_parts, string.format("📦 %s is a %d-byte binary file; binary content was not inserted into the model request", resolved_filename, #content))
				else
					table.insert(ui_parts, "📄 " .. resolved_filename)
					table.insert(llm_parts, string.format(
						"📄 %s\n─────────────\n%s\n─────────────",
						resolved_filename, content
					))
				end
			end
		end
		::continue::
	end

	local ui_value = table.concat(ui_parts, "\n")
	local llm_value = table.concat(llm_parts, "\n\n")
	if ctx.tool_args and #result_images > 0 then
		llm_value = {text = llm_value, images = result_images}
	end
	return ctx:replace(ui_value, llm_value)
end

plugin.tool = {
	name = "file_read",
	description = "Read a local file or list a local directory. Use path for one file; use paths to read several non-sensitive workspace files in one call. Use this for local file inspection instead of shell commands like cat, sed, or ls.",
	parameters = {
		type = "object",
		properties = {
			path = { type = "string", description = "Path to one file or directory to read" },
			paths = {
				type = "array",
				items = { type = "string" },
				description = "Several non-sensitive paths inside the workspace to read together"
			}
		},
		anyOf = {
			{ required = { "path" } },
			{ required = { "paths" } }
		}
	},
	permission = "file_read"
}

return plugin
