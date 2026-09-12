local plugin = {}
local redact = require("agent.redact")
local logging = require("agent.logging")
local tool_output = require("agent.tool_output")

plugin.id = "shell"
plugin.name = "Shell"
plugin.description = "Execute shell commands"
plugin.command = "/shell"
plugin.async = false

plugin.tool = {
	name = "shell",
	description = "Execute a shell command in a subprocess. Non-interactive (stdin is /dev/null). Returns redacted stdout, stderr, and exit code.",
	parameters = {
		type = "object",
		properties = {
			command = { type = "string", description = "The shell command to execute" },
			timeout = { type = "integer", description = "Timeout seconds (sync default 60; background default 0 unlimited; positive max 300)" },
			background = { type = "boolean", description = "Start a managed background process and return its ID immediately, not a completion or validation result." }
		},
		required = { "command" }
	},
	permission = "shell"
}

local function redact_secrets(text)
	return redact.text(text or "")
end

local function unquote_shell_token(token)
	if type(token) ~= "string" then return "" end
	if #token >= 2 then
		local first = token:sub(1, 1)
		local last = token:sub(-1)
		if (first == "'" and last == "'") or (first == '"' and last == '"') then
			return token:sub(2, -2)
		end
	end
	return token
end

local function summarize_shell_command(command)
	if type(command) ~= "string" or command == "" then
		return "shell"
	end
	local first = unquote_shell_token(command:match("^%s*([^%s]+)") or "")
	if first:match("/?curl$") or first == "curl" then
		for token in command:gmatch("%S+") do
			local clean_token = unquote_shell_token(token)
			if clean_token:match("^https?://") then
				return "curl " .. clean_token
			end
		end
		return "curl"
	end
	return redact_secrets(command)
end

local function trim(value)
	return tostring(value or ""):match("^%s*(.-)%s*$")
end

local function manual_command(ctx)
	if type(ctx.input) == "string" and type(ctx.command) == "string" then
		local _, command_end = ctx.input:find(ctx.command, 1, true)
		if command_end then
			return trim(ctx.input:sub(command_end + 1))
		end
	end
	return table.concat(ctx.args or {}, " ")
end

local function parse_manual_command(ctx)
	local command = manual_command(ctx)
	local timeout, background = nil, false
	while true do
		local rest = command:match("^%-%-background%s+(.*)$")
		if command == "--background" then rest = "" end
		if rest then
			background, command = true, trim(rest)
		else
			local parsed_timeout, tail = command:match("^%-%-timeout%s+(%d+)%s+(.+)$")
			if not parsed_timeout then
				parsed_timeout, tail = command:match("^%-t%s+(%d+)%s+(.+)$")
			end
			if not parsed_timeout then break end
			timeout, command = tonumber(parsed_timeout), trim(tail)
		end
	end
	return command, timeout, background
end

function plugin.handler(ctx)
	local command
	local timeout, background
	if ctx.tool_args and ctx.tool_args.command then
		command = ctx.tool_args.command
		timeout = tonumber(ctx.tool_args.timeout)
		background = ctx.tool_args.background == true
	else
		command, timeout, background = parse_manual_command(ctx)
	end
	timeout = timeout or (background and 0 or 60)

	if not command or command == "" then
		return ctx:replace("Usage: /shell <command>")
	end

	if timeout <= 0 then timeout = background and 0 or 60 end
	if timeout > 300 then timeout = 300 end

	local result = tools.shell(command, timeout, background == true)
	local display_command = summarize_shell_command(command)
	if background then
		if not result.started then
			return "Background shell failed to start", "Background shell failed to start", false
		end
		local out = string.format("[started] id=%s PID=%s status=%s\nNot a completion or validation result. Use processes to inspect output and exit status.",
			tostring(result.id), tostring(result.pid), tostring(result.status))
		logging.runtime_log("tool", "shell background " .. out)
		return "Shell: " .. display_command .. "\n" .. out, out, true
	end
	local redacted_stdout = redact_secrets(result.stdout or "")
	local redacted_stderr = redact_secrets(result.stderr or "")

	local out = string.format("[exit %d]", result.exit)
	if result.timed_out then
		out = out .. " TIMED OUT after " .. tostring(timeout) .. "s"
	end
	if #redacted_stdout > 0 then
		out = out .. "\n" .. redacted_stdout
	end
	if #redacted_stderr > 0 then
		out = out .. "\nstderr:\n" .. redacted_stderr
	end

	local visible_output = tool_output.bound(out)
	logging.runtime_log("tool", "shell result command=" .. display_command .. "\n" .. visible_output)
	local header = "Shell: " .. display_command .. "\n"
	return header .. visible_output,
		out,
		result.exit == 0 and not result.timed_out,
		{ shell_output_start = #header }
end

return plugin
