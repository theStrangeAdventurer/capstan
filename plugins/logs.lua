local plugin = {}

plugin.id = "logs"
plugin.name = "Logs"
plugin.description = "Show recent runtime log entries"
plugin.command = "/logs"
plugin.async = false

local function log_path()
	if capstan and capstan.log_path then
		return capstan.log_path()
	end
	if capstan and capstan.state_path then
		return capstan.state_path("events.log")
	end
	local home = os.getenv("HOME") or "."
	return home .. "/.local/state/capstan/events.log"
end

local function read_lines(path)
	local file = io.open(path, "r")
	if not file then
		return nil
	end

	local lines = {}
	for line in file:lines() do
		table.insert(lines, line)
	end
	file:close()
	return lines
end

function plugin.handler(ctx)
	local limit = tonumber(ctx.args[1]) or 80
	if limit <= 0 then limit = 80 end
	if limit > 500 then limit = 500 end

	local path = log_path()
	local lines = read_lines(path)
	if not lines then
		return ctx:replace("No log file yet: " .. path)
	end

	local start = #lines - limit + 1
	if start < 1 then start = 1 end

	local out = {}
	table.insert(out, "Log: " .. path)
	for i = start, #lines do
		table.insert(out, lines[i])
	end

	return ctx:replace(table.concat(out, "\n"))
end

return plugin
