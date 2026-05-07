local plugin = {}

plugin.id = "ask"
plugin.name = "Ask AI"
plugin.description = "Send a prompt to the LLM"
plugin.command = "/ask"
plugin.async = false

function plugin.handler(input)
    return input:match("^/ask%s+(.*)") or "Usage: /ask <question>"
end

return plugin
