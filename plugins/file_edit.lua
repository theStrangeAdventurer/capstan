local workspace = require("agent.workspace")

local plugin = {}

plugin.id = "file_edit"
plugin.name = "File Edit"
plugin.description = "Edit a file by replacing an exact text fragment"
plugin.command = "/edit"
plugin.async = false

plugin.tool = {
	name = "file_edit",
	description = "Replace an exact text fragment in an existing file. Use this for modifying existing files. It fails without writing if old_text is missing or appears more than once unless replace_all=true.",
	parameters = {
		type = "object",
		properties = {
			path = { type = "string", description = "Path to the file to edit. Relative paths resolve inside the active workspace." },
			old_text = { type = "string", description = "Exact existing text to replace" },
			new_text = { type = "string", description = "Replacement text" },
			replace_all = { type = "boolean", description = "When true, replace every occurrence of old_text. Default false requires exactly one match." }
		},
		required = { "path", "old_text", "new_text" }
	},
	permission = "file_write"
}

local function count_occurrences(haystack, needle)
	if needle == "" then
		return 0
	end
	local count = 0
	local start = 1
	while true do
		local found = haystack:find(needle, start, true)
		if not found then
			return count
		end
		count = count + 1
		start = found + #needle
	end
end

local function replace_once(haystack, old_text, new_text)
	local start_pos, end_pos = haystack:find(old_text, 1, true)
	if not start_pos then
		return haystack
	end
	return haystack:sub(1, start_pos - 1) .. new_text .. haystack:sub(end_pos + 1)
end

local function replace_all(haystack, old_text, new_text)
	local out = {}
	local start = 1
	while true do
		local start_pos, end_pos = haystack:find(old_text, start, true)
		if not start_pos then
			table.insert(out, haystack:sub(start))
			return table.concat(out)
		end
		table.insert(out, haystack:sub(start, start_pos - 1))
		table.insert(out, new_text)
		start = end_pos + 1
	end
end

local function line_number_at(text, pos)
	local line = 1
	local start = 1
	while true do
		local found = text:find("\n", start, true)
		if not found or found >= pos then
			return line
		end
		line = line + 1
		start = found + 1
	end
end

local function replacement_positions(content, old_text, replace_all_flag)
	local positions = {}
	local start = 1
	while true do
		local start_pos = content:find(old_text, start, true)
		if not start_pos then
			return positions
		end
		table.insert(positions, start_pos)
		if not replace_all_flag then
			return positions
		end
		start = start_pos + #old_text
	end
end

local function split_diff_lines(text)
	if text == "" then
		return {}
	end
	local lines = {}
	local start = 1
	while start <= #text do
		local newline = text:find("\n", start, true)
		if newline then
			table.insert(lines, text:sub(start, newline - 1))
			start = newline + 1
		else
			table.insert(lines, text:sub(start))
			break
		end
	end
	return lines
end

local function diff_range(count)
	if count == 1 then
		return ""
	end
	return "," .. tostring(count)
end

local function unified_edit_diff(path, content, old_text, new_text, replace_all_flag)
	local out = {
		"--- a/" .. path,
		"+++ b/" .. path,
	}
	local old_lines = split_diff_lines(old_text)
	local new_lines = split_diff_lines(new_text)
	for _, pos in ipairs(replacement_positions(content, old_text, replace_all_flag)) do
		local line = line_number_at(content, pos)
		table.insert(out, string.format("@@ -%d%s +%d%s @@", line, diff_range(#old_lines), line, diff_range(#new_lines)))
		for _, line_text in ipairs(old_lines) do
			table.insert(out, "-" .. line_text)
		end
		for _, line_text in ipairs(new_lines) do
			table.insert(out, "+" .. line_text)
		end
	end
	return table.concat(out, "\n")
end

function plugin.handler(ctx)
	local path = (ctx.tool_args and ctx.tool_args.path) or ctx.args[1]
	local old_text = (ctx.tool_args and ctx.tool_args.old_text) or ctx.args[2]
	local new_text = (ctx.tool_args and ctx.tool_args.new_text) or ctx.args[3]
	local replace_all_flag = ctx.tool_args and ctx.tool_args.replace_all == true

	if not path or tostring(path) == "" or old_text == nil or new_text == nil then
		return ctx:replace("Usage: /edit <path> <old_text> <new_text>")
	end

	path = tostring(path)
	old_text = tostring(old_text)
	new_text = tostring(new_text)
	if old_text == "" then
		return ctx:replace("Cannot edit " .. path .. ": old_text is empty")
	end

	if ctx.tool_args then
		local allowed, reason = workspace.model_path_allowed(path, "write")
		if not allowed then
			return ctx:replace("Cannot edit " .. workspace.resolve_path(path) .. ": " .. reason)
		end
	end

	local resolved_path = workspace.resolve_path(path)
	local raw_content, read_err = workspace.read_all(resolved_path)
	if not raw_content then
		return ctx:replace("Cannot edit " .. resolved_path .. ": " .. tostring(read_err))
	end

	local has_bom, content = workspace.split_utf8_bom(raw_content)
	local matches = count_occurrences(content, old_text)
	if matches == 0 then
		return ctx:replace("Cannot edit " .. resolved_path .. ": old_text not found")
	end
	if matches > 1 and not replace_all_flag then
		return ctx:replace("Cannot edit " .. resolved_path .. ": old_text matched " .. matches .. " times; pass replace_all=true to replace all")
	end

	local edited
	if replace_all_flag then
		edited = replace_all(content, old_text, new_text)
	else
		edited = replace_once(content, old_text, new_text)
	end

	local file, write_err = io.open(resolved_path, "wb")
	if not file then
		return ctx:replace("Cannot edit " .. resolved_path .. ": " .. tostring(write_err))
	end

	local ok
	if has_bom then
		ok, write_err = file:write(workspace.utf8_bom(), edited)
	else
		ok, write_err = file:write(edited)
	end
	local close_ok, close_err = file:close()
	if not ok then
		return ctx:replace("Cannot edit " .. resolved_path .. ": " .. tostring(write_err))
	end
	if not close_ok then
		return ctx:replace("Cannot edit " .. resolved_path .. ": " .. tostring(close_err))
	end

	local summary = string.format("Edited %s (%d replacement%s)", resolved_path, matches, matches == 1 and "" or "s")
	local diff = unified_edit_diff(path, content, old_text, new_text, replace_all_flag)
	return ctx:replace(summary .. "\n\n" .. diff)
end

return plugin
