local plugin = {}

plugin.id = "fetch"
plugin.name = "Fetch"
plugin.description = "Fetch a URL"
plugin.command = "/fetch"
plugin.async = false

plugin.tool = {
	name = "fetch",
	description = "Fetch an HTTP or HTTPS URL and return the response body.",
	parameters = {
		type = "object",
		properties = {
			url = { type = "string", description = "HTTP or HTTPS URL to fetch" }
		},
		required = { "url" }
	},
	permission = "fetch"
}

local function get_url(ctx)
	if ctx.tool_args and ctx.tool_args.url then
		return tostring(ctx.tool_args.url)
	end
	return ctx.args[1]
end

local function is_http_url(url)
	return type(url) == "string" and (url:match("^https://") or url:match("^http://"))
end

local function normalize_url(url)
	if type(url) ~= "string" or url == "" then
		return url
	end
	if is_http_url(url) then
		return url
	end
	if url:match("^[%w.-]+%.[%w.-]+") then
		return "https://" .. url
	end
	return url
end

local DEFAULT_USER_AGENT = "Capstan/1.0 (+https://github.com/theStrangeAdventurer/capstan)"

local function fetch_headers()
	local user_agent = os.getenv("CAPSTAN_PLUGIN_FETCH_UA")
	if not user_agent or user_agent == "" then
		user_agent = DEFAULT_USER_AGENT
	end
	return {
		["User-Agent"] = user_agent
	}
end

function plugin.handler(ctx)
	local input_url = get_url(ctx)
	if not input_url or input_url == "" then
		return ctx:replace("Usage: /fetch <url>")
	end

	local url = normalize_url(input_url)
	if not is_http_url(url) then
		return ctx:replace("Usage: /fetch <http-or-https-url>")
	end

	local status, body = http.get(url, fetch_headers())
	body = body or ""

	local llm_value = string.format("URL: %s\nStatus: %d\n\n%s", url, status, body)
	if status < 200 or status >= 300 then
		return ctx:replace(string.format("Fetch failed: %s (HTTP %d)", url, status), llm_value)
	end

	return ctx:replace(string.format("Fetched %s (HTTP %d, %d bytes)", url, status, #body), llm_value)
end

return plugin
