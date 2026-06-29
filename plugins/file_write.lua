local workspace = require("agent.workspace")

local plugin = {}

plugin.id = "file_write"
plugin.name = "File Write"
plugin.description = "Write content to a file"
plugin.command = "/write"
plugin.async = false

plugin.tool = {
	name = "file_write",
	description = "Write content to a file. Relative paths resolve inside the active workspace. Parent directories are created when needed. By default this overwrites the file; pass mode=\"append\" to add content to the end.",
	parameters = {
		type = "object",
		properties = {
			path = { type = "string", description = "Path to the file to write. Relative paths resolve inside the active workspace." },
			content = { type = "string", description = "Content to write to the file" },
			mode = { type = "string", enum = { "write", "append" }, description = "write overwrites the file; append adds content to the end." }
		},
		required = { "path", "content" }
	},
	permission = "file_write"
}

local function ensure_parent_dirs(path)
	local dir = workspace.dirname(path)
	if dir == "." or dir == "/" then
		return true
	end
	local ok = os.execute("mkdir -p -- " .. workspace.shell_quote(dir))
	if ok == true or ok == 0 then
		return true
	end
	return false, "mkdir failed for " .. dir
end

function plugin.handler(ctx)
	local arg_offset = 0
	local tool_args = type(ctx.tool_args) == "table" and ctx.tool_args or nil
	local mode = tool_args and tool_args.mode or "write"
	if not tool_args and ctx.args[1] == "--append" then
		mode = "append"
		arg_offset = 1
	end
	local path = tool_args and tool_args.path or ctx.args[1 + arg_offset]
	local content = tool_args and tool_args.content or ctx.args[2 + arg_offset]

	if not path or tostring(path) == "" then
		return ctx:replace("Usage: /write <path> <content>")
	end
	path = tostring(path)
	if not content then
		content = ""
	end
	content = tostring(content)
	if mode ~= "write" and mode ~= "append" then
		return ctx:replace("Usage: /write [--append] <path> <content>")
	end

	local resolved_path = workspace.resolve_path(path)
	local old_content = workspace.read_all(resolved_path)
	local existed = old_content ~= nil
	local old_has_bom = false
	if old_content then
		old_has_bom = workspace.split_utf8_bom(old_content)
	end
	local new_has_bom, new_text = workspace.split_utf8_bom(content)
	local write_bom = old_has_bom or new_has_bom

	local ok, mkdir_err = ensure_parent_dirs(resolved_path)
	if not ok then
		return ctx:replace("Cannot write " .. resolved_path .. ": " .. mkdir_err)
	end

	local file_mode = mode == "append" and "ab" or "wb"
	local file, err = io.open(resolved_path, file_mode)
	if not file then
		return ctx:replace("Cannot write " .. resolved_path .. ": " .. err)
	end

	local bytes, write_err
	if mode == "append" then
		bytes, write_err = file:write(new_text)
	elseif write_bom then
		bytes, write_err = file:write(workspace.utf8_bom(), new_text)
	else
		bytes, write_err = file:write(new_text)
	end
	local close_ok, close_err = file:close()

	if not bytes then
		return ctx:replace("Cannot write " .. resolved_path .. ": " .. tostring(write_err))
	end
	if not close_ok then
		return ctx:replace("Cannot write " .. resolved_path .. ": " .. tostring(close_err))
	end

	local action
	if mode == "append" then
		action = existed and "Appended to" or "Created"
	else
		action = existed and "Wrote" or "Created"
	end
	local size = #new_text + (mode ~= "append" and write_bom and #workspace.utf8_bom() or 0)
	local lines = workspace.line_count(new_text)
	local bom_note = mode ~= "append" and write_bom and ", UTF-8 BOM" or ""

	return ctx:replace(string.format("%s %s (%d bytes, %d lines%s)", action, resolved_path, size, lines, bom_note))
end

return plugin
