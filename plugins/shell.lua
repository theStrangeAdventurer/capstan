local plugin = {}

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
			timeout = { type = "integer", description = "Timeout in seconds (default 60, max 300)" }
		},
		required = { "command" }
	},
	permission = "shell"
}

local sensitive_headers = {
	authorization = true,
	["proxy-authorization"] = true,
	cookie = true,
	["set-cookie"] = true,
	["x-api-key"] = true,
	["api-key"] = true,
	["openai-api-key"] = true,
	["anthropic-api-key"] = true,
	["x-goog-api-key"] = true,
	["x-subscription-key"] = true,
	["subscription-key"] = true,
}

local sensitive_keys = {
	"api_key",
	"api-key",
	"apikey",
	"access_token",
	"access-token",
	"refresh_token",
	"refresh-token",
	"id_token",
	"id-token",
	"auth_token",
	"auth-token",
	"bearer_token",
	"bearer-token",
	"token",
	"secret",
	"password",
	"passwd",
}

local function redact_key_values(text)
	local result = text
	for _, key in ipairs(sensitive_keys) do
		result = result:gsub("([\"']?%f[%w]" .. key .. "%f[^%w][\"']?%s*[:=]%s*[\"']?)[^\"'%s,;}]+", "%1[REDACTED]")
		result = result:gsub("([\"']?%f[%w]" .. key:upper() .. "%f[^%w][\"']?%s*[:=]%s*[\"']?)[^\"'%s,;}]+", "%1[REDACTED]")
	end
	return result
end

local function redact_header_line(line)
	local curl_prefix, name = line:match("^(%s*[<>]%s*)([%w%-]+)%s*:")
	if curl_prefix and name then
		return curl_prefix .. name .. ": [REDACTED]"
	end
	name = line:match("^%s*([%w%-]+)%s*:")
	if not name then
		return line
	end
	if sensitive_headers[name:lower()] then
		return line:gsub("(:%s*).*$", "%1[REDACTED]")
	end
	return line
end

local function redact_secrets(text)
	if not text or text == "" then
		return text or ""
	end

	local result = text
	result = result:gsub("([Aa][Uu][Tt][Hh][Oo][Rr][Ii][Zz][Aa][Tt][Ii][Oo][Nn]%s*:%s*[Bb][Ee][Aa][Rr][Ee][Rr]%s+)[^%s\"']+", "%1[REDACTED]")
	result = result:gsub("([Aa][Uu][Tt][Hh][Oo][Rr][Ii][Zz][Aa][Tt][Ii][Oo][Nn]%s*:%s*)[^\r\n\"']+", "%1[REDACTED]")
	result = redact_key_values(result)

	local lines = {}
	local had_line = false
	for line, newline in result:gmatch("([^\r\n]*)(\r?\n?)") do
		if line == "" and newline == "" then
			break
		end
		had_line = true
		table.insert(lines, redact_header_line(line) .. newline)
	end
	if had_line then
		result = table.concat(lines)
	end
	return result
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
	local timeout = nil
	local parsed_timeout, rest = command:match("^%-%-timeout%s+(%d+)%s+(.+)$")
	if not parsed_timeout then
		parsed_timeout, rest = command:match("^%-t%s+(%d+)%s+(.+)$")
	end
	if parsed_timeout then
		timeout = tonumber(parsed_timeout)
		command = trim(rest)
	end
	return command, timeout
end

function plugin.handler(ctx)
	local command
	local timeout
	if ctx.tool_args and ctx.tool_args.command then
		command = ctx.tool_args.command
		timeout = tonumber(ctx.tool_args.timeout)
	else
		command, timeout = parse_manual_command(ctx)
	end
	timeout = timeout or 60

	if not command or command == "" then
		return ctx:replace("Usage: /shell <command>")
	end

	if timeout <= 0 then timeout = 60 end
	if timeout > 300 then timeout = 300 end

	local result = tools.shell(command, timeout)
	local display_command = summarize_shell_command(command)
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

	return ctx:replace(string.format("Shell: %s (exit %d)", display_command, result.exit), out)
end

return plugin
