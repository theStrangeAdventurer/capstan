local plugin = {}

-- Метаданные плагина
plugin.id = "greetings"
plugin.name = "Greetings"
plugin.description = "Replace greeting commands with emoji"
plugin.command = "/hi"
plugin.async = false

-- Обработчик команд
-- input: текущий текст ввода (может содержать команду)
function plugin.handler(input)
	local content_after_cmd = input:match(plugin.command .. '%s*(.*)$')
	local args_num = 1; -- name
	local args_res = {};
	local cmd_start = input:find(plugin.command, 1, true)
	local cmd_end = #plugin.command

	for word in content_after_cmd:gmatch('%S+') do
		if #args_res < args_num then
			table.insert(args_res, word)
			cmd_end = cmd_end + string.len(word) + 1
		end
	end

	local name = args_res[1];

	local plugin_result = "👋, " .. name .. "!"

	local result = input:sub(1, cmd_start - 1) .. plugin_result .. input:sub(cmd_end + 1)
	return result;
end

return plugin
