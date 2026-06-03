local plugin = {}

plugin.id = "ip"
plugin.name = "IP Lookup"
plugin.description = "Get your public IP address"
plugin.command = "/ip"
plugin.async = false

function plugin.handler(input)
	http.get("https://httpbin.org/delay/2") -- демо-блокировка чтобы увидеть спиннер
	local status, body = http.get("https://api.ipify.org")

	if status ~= 200 then
		return "/ip error: HTTP " .. status
	end

	local ip = body

	local ui_value = "🌐 " .. ip
	local llm_value = "My current public IP address is: " .. ip

	local cmd_start = input:find(plugin.command, 1, true)
	local cmd_end = cmd_start + #plugin.command
	local ui_result = input:sub(1, cmd_start - 1) .. ui_value .. input:sub(cmd_end + 1)
	local llm_result = input:sub(1, cmd_start - 1) .. llm_value .. input:sub(cmd_end + 1)

	return ui_result, llm_result
end

return plugin
