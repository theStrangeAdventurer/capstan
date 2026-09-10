local plugin = {
    id = 'show', name = 'Show Session', command = '/show-session', history = false,
    description = 'Show the session overlay',
}
function plugin.handler(ctx)
    if #ctx.args ~= 0 then
        return ctx:replace('Usage: /show-session', '')
    end
    agent.show_session()
    return ctx:replace('', '')
end
return plugin
