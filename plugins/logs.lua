local json = require("vendor.rxi.json")
local utf8_sanitize = require("agent.utf8")
local plugin = {}

plugin.id = "logs"
plugin.command = "/logs"
plugin.description = "Show recent Capstan runtime log lines."
plugin.tool = {
	name = "logs",
	description = "Read recent Capstan runtime log lines to debug failed tools, plugins, hooks, or API calls.",
	parameters = {
		type = "object",
		properties = {
			limit = {
				type = "integer",
				description = "Maximum number of recent log lines, default 80, maximum 500",
			},
		},
	},
	permission = false,
}

local MAX_LINES = 500
local DEFAULT_LIMIT = 80
local MAX_READ_BYTES = 4 * 1024 * 1024
local MAX_FILE_READ_BYTES = 1024 * 1024
local MAX_LINE_BYTES = 16 * 1024

local function log_path()
	if capstan and capstan.log_path then
		return capstan.log_path()
	end
	if capstan and capstan.state_path then
		return capstan.state_path("logs/" .. os.date("%Y-%m-%d") .. ".jsonl")
	end
	local home = os.getenv("HOME") or "."
	return home .. "/.local/state/capstan/logs/" .. os.date("%Y-%m-%d") .. ".jsonl"
end

local function single_line(value)
	local sanitized = utf8_sanitize.sanitize(tostring(value or ""))
	local parts = {}
	for _, codepoint in utf8.codes(sanitized) do
		if codepoint == 9 or codepoint == 10 or codepoint == 13 then
			table.insert(parts, " ")
		elseif codepoint <= 0x1F or codepoint == 0x7F or
			(codepoint >= 0x80 and codepoint <= 0x9F) then
			table.insert(parts, string.format("\\u%04X", codepoint))
		else
			table.insert(parts, utf8.char(codepoint))
		end
	end
	return table.concat(parts)
end

local function bound_line(line)
	if #line <= MAX_LINE_BYTES then return line end
	return line:sub(1, MAX_LINE_BYTES) ..
		string.format("...<log record truncated; %d bytes>", #line)
end

local function display_line(line)
	local ok, event = pcall(json.decode, line)
	if not ok or type(event) ~= "table" or event.schema ~= "capstan.log.v1" then
		return single_line(bound_line(line))
	end
	local parts = {single_line(event.timestamp or "unknown-time")}
	if event.session_id then
		table.insert(parts, "[session:" .. single_line(event.session_id) .. "]")
	end
	table.insert(parts, "[" .. single_line(event.level or "info") .. "]")
	table.insert(parts, "[" .. single_line(event.category or "event") .. "]")
	table.insert(parts, single_line(event.message or ""))
	return bound_line(table.concat(parts, " "))
end

local function candidate_paths(path)
	local paths = {path}
	if path:match("%.jsonl$") then
		for index = 1, 5 do
			table.insert(paths, (path:gsub("%.jsonl$", "." .. index .. ".jsonl")))
		end
		local legacy = path:gsub("%.jsonl$", ".log")
		table.insert(paths, legacy)
		for index = 1, 5 do
			table.insert(paths, (legacy:gsub("%.log$", "." .. index .. ".log")))
		end
	end
	return paths
end

local function tail_file(path, capacity, byte_budget)
	local amount = math.min(byte_budget, MAX_FILE_READ_BYTES)
	local content, start = capstan.log_read_tail(path, amount)
	if content == nil then return nil, 0 end
	local consumed = #content
	local oversized_prefix = false
	if start > 0 then
		local newline = content:find("\n", 1, true)
		if newline then
			content = content:sub(newline + 1)
		else
			content = ""
			oversized_prefix = true
		end
	end
	if content:sub(-1) ~= "\n" then
		local last_newline = content:match(".*()\n")
		content = last_newline and content:sub(1, last_newline) or ""
	end

	local lines = {}
	for line in content:gmatch("([^\n]*)\n") do
		table.insert(lines, (line:gsub("\r$", "")))
		if #lines > capacity then table.remove(lines, 1) end
	end
	if oversized_prefix and #lines < capacity then
		table.insert(lines, 1, "[oversized log record omitted]")
	end
	return lines, consumed
end

local function read_recent_raw_lines(path, capacity)
	local selected = {}
	local found = false
	local remaining_bytes = MAX_READ_BYTES
	for _, candidate in ipairs(candidate_paths(path)) do
		if #selected >= capacity or remaining_bytes <= 0 then break end
		local lines, consumed = tail_file(candidate, capacity - #selected, remaining_bytes)
		if lines then
			found = true
			remaining_bytes = remaining_bytes - consumed
			local combined = {}
			for _, line in ipairs(lines) do table.insert(combined, line) end
			for _, line in ipairs(selected) do table.insert(combined, line) end
			selected = combined
		end
	end
	if not found then return nil end
	return selected
end

local function normalized_limit(value)
	local limit = tonumber(value)
	if not limit or limit ~= limit or limit == math.huge or limit == -math.huge or
		limit % 1 ~= 0 then
		return DEFAULT_LIMIT
	end
	return math.max(1, math.min(MAX_LINES, limit))
end

function plugin.handler(ctx)
	local requested = ctx.tool_args and ctx.tool_args.limit or ctx.args[1]
	local limit = normalized_limit(requested)
	local path = log_path()
	local has_log_api = capstan and capstan.log_read_lock and
		capstan.log_read_unlock and capstan.log_read_tail
	if not has_log_api then error("Runtime log read API is unavailable") end
	local locked, locked_path = capstan.log_read_lock()
	if not locked then error("Could not lock runtime logs for reading") end
	if type(locked_path) == "string" and locked_path ~= "" then
		path = locked_path
	end
	local ok, raw_lines = pcall(read_recent_raw_lines, path, MAX_LINES)
	if locked then capstan.log_read_unlock() end
	if not ok then error(raw_lines) end
	if not raw_lines then
		return ctx:replace("No log file yet: " .. single_line(path))
	end

	local first = math.max(1, #raw_lines - limit + 1)
	local lines = {}
	for index = first, #raw_lines do
		table.insert(lines, display_line(raw_lines[index]))
	end
	return ctx:replace("Runtime log: " .. single_line(path) .. "\n\n" ..
		table.concat(lines, "\n"))
end

return plugin
