# 1. Файл на выходе
Output docs/assets/demo.gif
Set Theme { "background": "#1a1f26", "foreground": "#a8b5c3", "cursor": "#7cb3b3", "black": "#141820", "brightBlack": "#3e4a56", "red": "#c97575", "brightRed": "#d89494", "green": "#6ba877", "brightGreen": "#89c994", "yellow": "#e8c574", "brightYellow": "#f0d89a", "blue": "#5a8fa1", "brightBlue": "#7ab0c4", "magenta": "#a97fbf", "brightMagenta": "#c898d9", "cyan": "#6b9f8a", "brightCyan": "#8ab9a2", "white": "#a8b5c3", "brightWhite": "#cad4e3" }
# 2. Настройки окна (Только валидные параметры для VHS)
#Set FontSize 24
#Set FontFamily "JetBrains Mono"
Set Width 2200
Set Height 1200
Set WindowBarSize 80      # Увеличиваем саму плашку с кнопками (дефолт 40 — слишком мало)
Set BorderRadius 16       # Красивое, современное скругление углов самого терминала

Set Padding 60            # Внутренний отступ от текста до краев терминала
Set Margin 40             # Внешний отступ (рамка) вокруг самого терминала
Set MarginFill "#241133"  # Dark purple background fill — no file required
# Стиль WindowBar "Colorful" добавит те самые 3 цветные кнопки (как на macOS)
Set WindowBar Colorful

# 3. Сценарий действий (Запускаем бинарь и прогоняем ваш сценарий)

# Шаг 0: Запуск вашего бинарника (замените на имя вашего файла, например "./capstan")
Type "capstan"
Enter
Sleep 3.5s

# --- ВАШ СЦЕНАРИЙ ---

# Вбиваем /
Type "/"
Sleep 500ms

# Вбиваем mcp
Type@100ms "mcp"
Sleep 400ms

# Нажимаем enter
Enter
Sleep 500ms

# Нажимаем enter
Enter
Sleep 1s

# Вбиваем промпт на английском
Type@80ms "Tell me about yourself, what tools do you have, and what kind of project is this?"
Sleep 400ms

# Финальный Enter, чтобы отправить промпт агенту
Enter

# Даем агенту 8 секунд, чтобы он вывел ответ на экран перед завершением гифки
Sleep 15s
