local plugin = {}

-- Метаданные плагина
plugin.id = "greetings"
plugin.name = "Greetings"
plugin.description = "Replace greeting commands with emoji"
plugin.command = "/hi"
plugin.async = false

-- Обработчик команд
function plugin.handler(input, command, args)
	-- input: текущий текст ввода (может содержать команду)
	-- command: команда, которую нужно обработать
	-- args: таблица аргументов команды

	return "👋"
end

return plugin
