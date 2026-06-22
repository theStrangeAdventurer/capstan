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

local UTF8_BOM = string.char(0xef, 0xbb, 0xbf)

local function is_absolute(path)
	return path:sub(1, 1) == "/"
end

local function configured_workdir()
	if _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir:sub(1, 1) == "/" then
		return _G.capstan.workdir
	end
	local env = os.getenv("CAPSTAN_WORKDIR") or os.getenv("CAPSTAN_WORKSPACE")
	if env and env:sub(1, 1) == "/" then
		return env
	end
	local pwd = os.getenv("PWD")
	if pwd and pwd:sub(1, 1) == "/" then
		return pwd
	end
	return "."
end

local function resolve_path(path)
	if is_absolute(path) then
		return path
	end
	return configured_workdir():gsub("/+$", "") .. "/" .. path
end

local function shell_quote(value)
	return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function dirname(path)
	local dir = path:match("^(.*)/[^/]*$")
	if not dir or dir == "" then
		return "."
	end
	return dir
end

local function ensure_parent_dirs(path)
	local dir = dirname(path)
	if dir == "." or dir == "/" then
		return true
	end
	local ok = os.execute("mkdir -p -- " .. shell_quote(dir))
	if ok == true or ok == 0 then
		return true
	end
	return false, "mkdir failed for " .. dir
end

local function read_all(path)
	local file = io.open(path, "rb")
	if not file then
		return nil
	end
	local content = file:read("*a") or ""
	file:close()
	return content
end

local function split_bom(content)
	content = content or ""
	if content:sub(1, #UTF8_BOM) == UTF8_BOM then
		return true, content:sub(#UTF8_BOM + 1)
	end
	return false, content
end

local function line_count(content)
	if content == "" then
		return 0
	end
	local lines = 1
	for _ in content:gmatch("\n") do
		lines = lines + 1
	end
	return lines
end

function plugin.handler(ctx)
	local mode = (ctx.tool_args and ctx.tool_args.mode) or "write"
	local arg_offset = 0
	if ctx.args[1] == "--append" then
		mode = "append"
		arg_offset = 1
	end
	local path = ctx.args[1 + arg_offset] or (ctx.tool_args and ctx.tool_args.path)
	local content = ctx.args[2 + arg_offset] or (ctx.tool_args and ctx.tool_args.content)

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

	local resolved_path = resolve_path(path)
	local old_content = read_all(resolved_path)
	local existed = old_content ~= nil
	local old_has_bom = false
	if old_content then
		old_has_bom = split_bom(old_content)
	end
	local new_has_bom, new_text = split_bom(content)
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
		bytes, write_err = file:write(UTF8_BOM, new_text)
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
	local size = #new_text + (mode ~= "append" and write_bom and #UTF8_BOM or 0)
	local lines = line_count(new_text)
	local bom_note = mode ~= "append" and write_bom and ", UTF-8 BOM" or ""

	return ctx:replace(string.format("%s %s (%d bytes, %d lines%s)", action, resolved_path, size, lines, bom_note))
end

return plugin
