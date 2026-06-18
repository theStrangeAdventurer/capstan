local plugin = {}
local json = require("vendor.rxi.json")

plugin.id = "post_example"
plugin.name = "POST req example"
plugin.description = "Demonstrate http.post with JSON body"
plugin.command = "/post"
plugin.async = false

function plugin.handler(ctx)
	local payload = json.encode({
		message = "Hello from capstan",
		timestamp = os.time()
	})

	local status, body = http.post(
		"https://httpbin.org/post",
		payload,
		{["Content-Type"] = "application/json"}
	)

	if status ~= 200 then
		return "/post error: HTTP " .. status
	end

	local response = json.decode(body)

	return ctx:replace(
		"POST " .. status .. " | echo: " .. response.json.message,
		"HTTP POST to httpbin returned status " .. status .. " with message: " .. response.json.message
	)
end

return plugin
