# План реализации Lua-плагинов для TUI приложения

## Текущее состояние проекта
- TUI приложение на C с ncurses
- Статическая сборка (ncurses + Lua 5.5.0 включены)
- Базовая функция `replace_with` для замены строк
- Заголовочный файл `plugins.h` с структурой `PluginCommand`

## Требования
1. **Тип плагинов**: только `command`
2. **Приоритетные плагины**: 
   - `/hi` → `👋` (синхронный)
   - `/file <path>` → содержимое файла (асинхронный)
   - `/fetch <url>` → содержимое страницы (асинхронный)
3. **Асинхронность**: неблокирующий I/O
4. **UI-индикация**: спиннер в углу + текст ошибки в том же месте
5. **Обработка ошибок**: показ кратких сообщений об ошибках

## Архитектура

### 1. Система плагинов

#### 1.1 Структура Plugin
```c
// include/plugins.h
typedef enum {
    PLUGIN_SYNC,     // Мгновенный результат (hi, time)
    PLUGIN_ASYNC     // Асинхронный результат (file, fetch)
} PluginSyncType;

typedef struct Plugin Plugin;
typedef void (*PluginCallback)(Plugin *plugin, const char *result, void *user_data);

typedef struct {
    // Метаданные
    char *id;
    char *name;
    char *description;
    PluginSyncType sync_type;
    
    // Обработчик
    lua_State *L;
    int handler_ref;
    
    // Для асинхронных операций
    int async_id;           // ID асинхронной операции
    int is_processing;      // Флаг выполнения
    char *pending_result;   // Ожидаемый результат
    PluginCallback callback;// Коллбек для результата
    void *user_data;        // Пользовательские данные
} Plugin;
```

#### 1.2 Система управления асинхронными задачами
```c
// include/async.h
typedef struct AsyncTask AsyncTask;
typedef void (*AsyncTaskCallback)(AsyncTask *task, const char *result);

struct AsyncTask {
    int id;
    Plugin *plugin;
    char *command;
    char **args;
    int status;                 // PENDING, PROCESSING, COMPLETED, FAILED
    char *error_message;        // Сообщение об ошибке
    AsyncTaskCallback callback;
    void *user_data;
    
    // Для сетевых операций
    int fd;                     // Файловый дескриптор сокета
    // Для файловых операций  
    FILE *file_handle;
    // Таймаут
    time_t start_time;
    int timeout_ms;
};

// Очередь задач и менеджер
typedef struct {
    AsyncTask **tasks;
    int max_tasks;
    int current_id;
} AsyncManager;
```

### 2. Модификация TUI для асинхронности

#### 2.1 Неблокирующий ввод в main loop
```c
// main.c: измененный основной цикл
timeout(0);  // Неблокирующий getch()
while (1) {
    refresh();
    int ch = getch();
    
    if (ch == ERR) {
        // Проверить асинхронные задачи
        async_manager_check_tasks();
        
        // Отобразить прогресс/ошибки
        display_async_status();
        
        napms(10);  // 10ms задержка
        continue;
    }
    
    // Обработка ввода...
}
```

#### 2.2 Функция отображения статуса
```c
// tui.c: дополнение
void display_async_status(int x, int y) {
    if (async_has_active_tasks()) {
        // Показать спиннер
        static int spinner_frame = 0;
        char spinner[] = "|/-\\";
        mvprintw(y, x, "%c Processing...", spinner[spinner_frame++ % 4]);
    } else if (async_has_error()) {
        // Показать ошибку
        mvprintw(y, x, "Error: %s", async_get_last_error());
    }
}
```

### 3. Структура Lua-плагинов

#### 3.1 Простые синхронные плагины
```lua
-- plugins/hi.lua
local plugin = {}

plugin.id = "greetings"
plugin.name = "Greetings"
plugin.description = "Replace /hi with waving hand"
plugin.sync_type = "sync"

plugin.commands = {"/hi", "/hello"}

function plugin.handler(input, command, args)
    return "👋"
end

return plugin
```

#### 3.2 Асинхронный файловый плагин
```lua
-- plugins/file.lua
local plugin = {}

plugin.id = "file-reader"
plugin.name = "File Reader"
plugin.description = "Read file contents"
plugin.sync_type = "async"

plugin.commands = {"/file", "/read"}

function plugin.handler(input, command, args, async_context)
    local filename = args[1]
    if not filename then
        return nil, "Missing filename"
    end
    
    -- Запускаем асинхронное чтение
    async_read_file(filename, function(content, err)
        if err then
            async_context:complete(nil, "Cannot read file: " .. err)
        else
            async_context:complete(content)
        end
    end)
    
    return nil  -- Результат будет позже через коллбек
end

return plugin
```

#### 3.3 Асинхронный сетевой плагин
```lua
-- plugins/fetch.lua
local plugin = {}

plugin.id = "http-fetch"
plugin.name = "HTTP Fetcher"
plugin.description = "Fetch URL content"
plugin.sync_type = "async"

plugin.commands = {"/fetch", "/http"}

function plugin.handler(input, command, args, async_context)
    local url = args[1]
    if not url then
        return nil, "Missing URL"
    end
    
    async_http_get(url, function(response, err)
        if err then
            async_context:complete(nil, "HTTP error: " .. err)
        else
            async_context:complete(response)
        end
    end)
    
    return nil
end

return plugin
```

### 4. C-Lua биндинги для асинхронности

#### 4.1 API для плагинов в Lua
```c
// plugins.c: регистрация функций в Lua
lua_pushcfunction(L, lua_async_read_file);
lua_setglobal(L, "async_read_file");

lua_pushcfunction(L, lua_async_http_get);
lua_setglobal(L, "async_http_get");
```

#### 4.2 Неблокирующий I/O реализация
```c
// network.c: для асинхронных сетевых операций
int async_http_get_start(const char *url, AsyncTask *task) {
    // 1. Разобрать URL
    // 2. Создать неблокирующий сокет
    // 3. Подключиться асинхронно
    // 4. Добавить сокет в fd_set для select
}

// file.c: для асинхронных файловых операций
int async_file_read_start(const char *path, AsyncTask *task) {
    // Использовать aio или потоки для неблокирующего чтения
}
```

### 5. Обработка ошибок и статусов

#### 5.1 Система сообщений
```c
// error.c: управление ошибками
typedef struct {
    char *message;
    time_t timestamp;
    int duration_ms;  // Сколько показывать
} StatusMessage;

void status_show(const char *format, ...);
void status_show_error(const char *format, ...);
void status_clear();
```

#### 5.2 Отображение в UI
```c
// tui.c: дополнение redraw
void redraw_with_status(int x, int y, char *input) {
    // Основной текст
    mvprintw(y, x, "%s", input);
    
    // Статус в правом верхнем углу
    int status_x = getmaxx(stdscr) - 30;
    int status_y = 0;
    
    char *status = status_get_current();
    if (status) {
        mvprintw(status_y, status_x, "%s", status);
    }
}
```

## План по этапам

### Неделя 1: Базовые структуры и синхронные плагины
- [ ] Этап 1.1: Расширенные структуры данных плагинов
- [ ] Этап 1.2: Базовый Lua биндинг (загрузка файлов, выполнение кода)
- [ ] Этап 2.1: Модификация main loop для неблокирующего ввода
- [ ] Плагин `/hi` (синхронный): полная реализация
- [ ] Система парсинга команд (разбор `/команда аргумент1 аргумент2`)

### Неделя 2: Асинхронные операции с файлами
- [ ] Этап 3.1: Система асинхронных задач (AsyncManager)
- [ ] Этап 4.1: Асинхронные файловые операции
- [ ] Этап 5.1: Система статусов и ошибок
- [ ] Плагин `/file <path>` (асинхронный): полная реализация
- [ ] UI спиннер и отображение прогресса

### Неделя 3: Сетевые операции
- [ ] Этап 4.2: Асинхронные сетевые операции (неблокирующие сокеты)
- [ ] Плагин `/fetch <url>` (асинхронный): полная реализация
- [ ] Таймауты и обработка сетевых ошибок
- [ ] Оптимизация производительности select/poll цикла

### Неделя 4: Интеграция и тестирование
- [ ] Этап 5.2: Полная интеграция всех компонентов
- [ ] Тестирование с различными сценариями
- [ ] Документация API для разработчиков плагинов
- [ ] Создание примеров плагинов
- [ ] Финальная оптимизация и тесты производительности

## Модули проекта

### Существующие:
- `src/main.c` - основной цикл приложения
- `src/tui.c` - функции отрисовки UI
- `src/utils.c` - утилиты (включая `replace_with`)
- `include/tui.h`, `include/utils.h` - заголовочные файлы
- `include/plugins.h` - структуры плагинов (расширить)

### Новые модули:
- `src/plugins.c` - управление плагинами, Lua биндинги
- `src/async.c` - менеджер асинхронных задач
- `src/network.c` - асинхронные сетевые операции
- `src/file_async.c` - асинхронные файловые операции  
- `src/error.c` - система сообщений об ошибках и статусов
- `include/async.h`, `include/network.h`, `include/error.h` - заголовочные файлы

### Директории:
- `plugins/` - директория для Lua-плагинов
  - `hi.lua` - плагин приветствия
  - `file.lua` - плагин чтения файлов
  - `fetch.lua` - плагин HTTP запросов

## Конфигурационные параметры

### Ограничения:
- Максимальный размер файла для `/file`: 10MB
- Таймаут для `/fetch`: 10 секунд
- Максимальное количество одновременных задач: 5
- Максимальная длина вывода плагина: 4096 символов

### Форматирование:
- Результат `/fetch`: обрезать HTML теги, показывать первые 2000 символов
- Бинарные файлы: показывать "[Binary file]" вместо содержимого
- Длинный вывод: добавлять "..." в конце при обрезке

## Вопросы для дальнейшего развития

### Безопасность:
1. Ограничить доступ плагинов к файловой системе (только определенные директории)
2. Проверять URL в `/fetch` на безопасность (запретить localhost, private сети)
3. Добавить систему разрешений для плагинов

### Расширение функциональности:
1. Команда `/plugins` для вывода списка доступных плагинов
2. Горячая перезагрузка плагинов без перезапуска приложения
3. Система логгирования операций плагинов
4. Поддержка конфигурационных файлов для плагинов

### UI улучшения:
1. Автодополнение команд по Tab
2. История команд
3. Подсветка синтаксиса для разных типов вывода
4. Режим многострочного ввода

## Технические детали реализации

### Неблокирующий I/O:
1. Использовать `select()` для мониторинга сокетов и файловых дескрипторов
2. Для файловых операций использовать либо AIO, либо рабочие потоки
3. Таймауты для всех асинхронных операций
4. Буферизация ввода/вывода для сетевых операций

### Работа с Lua:
1. Загружать каждый плагин в отдельный Lua state для изоляции
2. Использовать пул Lua states для производительности
3. Регистрировать безопасное API для Lua плагинов
4. Обрабатывать ошибки выполнения Lua кода

### Производительность:
1. Кэшировать метаданные плагинов
2. Оптимизировать парсинг команд
3. Использовать пул буферов для результатов
4. Минимизировать аллокации памяти в основном цикле