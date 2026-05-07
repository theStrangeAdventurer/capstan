local plugin = {}
local json = require("vendor.rxi.json")

plugin.id = "post_example"
plugin.name = "POST req example"
plugin.description = "Demonstrate http.post with JSON body"
plugin.command = "/post"
plugin.async = false

function plugin.handler(input)
	local payload = json.encode({
		message = "Hello from termai",
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

	local ui_value = "POST " .. status .. " | echo: " .. response.json.message
	local llm_value = "HTTP POST to httpbin returned status " .. status .. " with message: " .. response.json.message

	local cmd_start = input:find(plugin.command, 1, true)
	local cmd_end = cmd_start + #plugin.command
	local ui_result = input:sub(1, cmd_start - 1) .. ui_value .. input:sub(cmd_end + 1)
	local llm_result = input:sub(1, cmd_start - 1) .. llm_value .. input:sub(cmd_end + 1)

	return ui_result, llm_result
end

return plugin
