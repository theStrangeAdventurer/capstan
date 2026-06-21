local plugin = {}

plugin.id = "file_write"
plugin.name = "File Write"
plugin.description = "Write content to a file"
plugin.command = "/write"
plugin.async = false

plugin.tool = {
	name = "file_write",
	description = "Write or overwrite content to a file at the given path",
	parameters = {
		type = "object",
		properties = {
			path = { type = "string", description = "Path to the file to write" },
			content = { type = "string", description = "Content to write to the file" }
		},
		required = { "path", "content" }
	},
	permission = "file_write"
}

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

function plugin.handler(ctx)
	local path = ctx.args[1] or (ctx.tool_args and ctx.tool_args.path)
	local content = ctx.args[2] or (ctx.tool_args and ctx.tool_args.content)

	if not path then
		return ctx:replace("Usage: /write <path> <content>")
	end
	if not content then
		content = ""
	end

	local resolved_path = resolve_path(path)
	local file, err = io.open(resolved_path, "w")
	if not file then
		return ctx:replace("❌ Cannot write " .. resolved_path .. ": " .. err)
	end

	file:write(content)
	file:close()

	local lines = 0
	for _ in content:gmatch("\n") do lines = lines + 1 end

	return ctx:replace("📄 Wrote " .. resolved_path .. " (" .. #content .. " bytes, " .. (lines + 1) .. " lines)")
end

return plugin
