local plugin = {}

plugin.id = "ip"
plugin.name = "IP Lookup"
plugin.description = "Get your public IP address"
plugin.command = "/ip"
plugin.async = false

function plugin.handler(ctx)
	http.get("https://httpbin.org/delay/2") -- демо-блокировка чтобы увидеть спиннер
	local status, body = http.get("https://api.ipify.org")

	if status ~= 200 then
		return "/ip error: HTTP " .. status
	end

	return ctx:replace("🌐 " .. body, "My current public IP address is: " .. body)
end

return plugin
