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

function plugin.handler(ctx)
	local path = ctx.args[1] or (ctx.tool_args and ctx.tool_args.path)
	local content = ctx.args[2] or (ctx.tool_args and ctx.tool_args.content)

	if not path then
		return ctx:replace("Usage: /write <path> <content>")
	end
	if not content then
		content = ""
	end

	local file, err = io.open(path, "w")
	if not file then
		return ctx:replace("❌ Cannot write " .. path .. ": " .. err)
	end

	file:write(content)
	file:close()

	local lines = 0
	for _ in content:gmatch("\n") do lines = lines + 1 end

	return ctx:replace("📄 Wrote " .. path .. " (" .. #content .. " bytes, " .. (lines + 1) .. " lines)")
end

return plugin
