local plugin = {}

-- Метаданные плагина
plugin.id = "file"
plugin.name = "File"
plugin.description = "Read file content"
plugin.command = "/file"
plugin.async = false

-- Обработчик команд
-- input: текущий текст ввода вместе с командой
function plugin.handler(input)
	local filename = "README.md"
	local file = io.open(filename, "r") -- "r" = читать
	if not file then
		return "Cannot open file"
	end
	local file_content = file:read("*a") -- "*a" = читать весь файл
	local llm_value = string.format("📄 %s\n─────────────\n%s\n─────────────",
		filename, file_content)
	local ui_value = "📄[" .. filename .. "]"
	local cmd_start = input:find(plugin.command, 1, true)
	local cmd_end = cmd_start + #plugin.command
	local ui_result = input:sub(1, cmd_start - 1) .. ui_value .. " " .. input:sub(cmd_end + 1)
	local llm_result = input:sub(1, cmd_start - 1) .. llm_value .. " " .. input:sub(cmd_end + 1)

	file:close()
	return ui_result, llm_result
end

return plugin
