local logging = require("agent.logging")

local M = {
    registry = {},
    seq = 0,
}

local function ensure_capstan_hooks()
    if not _G.capstan then _G.capstan = {} end
    _G.capstan.hooks = {
        register = function(name, handler, opts)
            return M.register(name, handler, opts)
        end,
        remove_source = function(source)
            return M.remove_source(source)
        end,
        install_plugin = function(plugin)
            return M.install_plugin(plugin)
        end,
    }
end

local function hook_source(opts)
    if type(opts) == "table" and opts.source then
        return tostring(opts.source)
    end
    return "unknown"
end

function M.register(name, handler, opts)
    if type(name) ~= "string" or name == "" or type(handler) ~= "function" then
        return false
    end
    opts = type(opts) == "table" and opts or {}
    M.seq = M.seq + 1
    local hooks = M.registry[name]
    if not hooks then
        hooks = {}
        M.registry[name] = hooks
    end
    table.insert(hooks, {
        handler = handler,
        priority = tonumber(opts.priority) or 100,
        seq = M.seq,
        source = hook_source(opts),
    })
    table.sort(hooks, function(a, b)
        if a.priority == b.priority then
            return a.seq < b.seq
        end
        return a.priority < b.priority
    end)
    return true
end

function M.has(name)
    local hooks = M.registry[name]
    return hooks ~= nil and #hooks > 0
end

function M.remove_source(source)
    if not source then return end
    source = tostring(source)
    for name, hooks in pairs(M.registry) do
        local kept = {}
        for _, hook in ipairs(hooks) do
            if hook.source ~= source then
                table.insert(kept, hook)
            end
        end
        M.registry[name] = kept
    end
end

function M.run(name, ctx)
    local hooks = M.registry[name]
    if not hooks or #hooks == 0 then
        return ctx
    end
    ctx = ctx or {}
    for _, hook in ipairs(hooks) do
        local ok, result = pcall(hook.handler, ctx)
        if ok then
            if result ~= nil then
                ctx = result
            end
        else
            logging.runtime_log("hook", string.format(
                "error stage=%s source=%s message=%s",
                name,
                hook.source,
                logging.compact(result, 300)
            ))
        end
    end
    return ctx
end

local function install_entry(name, entry, source)
    if type(entry) == "function" then
        M.register(name, entry, {source = source})
    elseif type(entry) == "table" then
        if type(entry.handler) == "function" then
            M.register(name, entry.handler, {
                priority = entry.priority,
                source = entry.source or source,
            })
        else
            for _, nested in ipairs(entry) do
                install_entry(name, nested, source)
            end
        end
    end
end

function M.install_table(hooks_table, source)
    if type(hooks_table) ~= "table" then return end
    for name, entry in pairs(hooks_table) do
        if type(name) == "string" then
            install_entry(name, entry, source)
        end
    end
end

function M.install_config(config)
    if type(config) == "table" then
        M.install_table(config.hooks, "config")
    end
end

function M.install_plugin(plugin, id_override)
    if type(plugin) ~= "table" then return end
    local id = tostring(plugin.id or id_override or "plugin")
    local source = "plugin:" .. id
    M.remove_source(source)
    M.install_table(plugin.hooks, source)
end

function M.install_existing_plugins(plugins)
    if type(plugins) ~= "table" then return end
    for id, plugin in pairs(plugins) do
        M.install_plugin(plugin, id)
    end
end

ensure_capstan_hooks()

return M
