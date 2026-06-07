local plugin = {}

plugin.id = "shell"
plugin.name = "Shell"
plugin.description = "Execute shell commands"
plugin.command = "/shell"
plugin.async = false

plugin.tool = {
	name = "shell",
	description = "Execute a shell command in a subprocess. Non-interactive (stdin is /dev/null). Returns stdout, stderr, and exit code.",
	parameters = {
		type = "object",
		properties = {
			command = { type = "string", description = "The shell command to execute" },
			timeout = { type = "integer", description = "Timeout in seconds (default 60, max 300)" }
		},
		required = { "command" }
	},
	permission = "shell"
}

function plugin.handler(ctx)
	local command = ctx.args[1] or (ctx.tool_args and ctx.tool_args.command)
	local timeout = tonumber(ctx.args[2]) or (ctx.tool_args and ctx.tool_args.timeout) or 60

	if not command then
		return ctx:replace("Usage: /shell <command>")
	end

	if timeout <= 0 then timeout = 60 end
	if timeout > 300 then timeout = 300 end

	local result = tools.shell(command, timeout)

	local out = string.format("[exit %d]", result.exit)
	if result.timed_out then
		out = out .. " TIMED OUT after " .. tostring(timeout) .. "s"
	end
	if #result.stdout > 0 then
		out = out .. "\n" .. result.stdout
	end
	if #result.stderr > 0 then
		out = out .. "\nstderr:\n" .. result.stderr
	end

	return ctx:replace(string.format("Shell: %s (exit %d)", command, result.exit), out)
end

return plugin
