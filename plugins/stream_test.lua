local plugin = {}
local json = require("vendor.rxi.json")

plugin.id = "stream_test"
plugin.name = "Stream POST test"
plugin.description = "Test http.post_stream -> agent.append pipeline"
plugin.command = "/stream"
plugin.async = false

function plugin.handler(ctx)
	_G.stream_done = false

	http.post_stream(
		"https://httpbin.org/post",
		json.encode({msg = "streaming from termai", ts = os.time()}),
		{["Content-Type"] = "application/json"},
		function(raw, is_done)
			if is_done then
				_G.stream_done = true
			else
				agent.append(raw)
			end
		end
	)

	return "stream started..."
end

return plugin
