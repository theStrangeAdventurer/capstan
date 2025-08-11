local M = {}

function M.append(text, role)
    if not agent then return end
    local append = type(agent.append_ui) == "function" and agent.append_ui or agent.append
    if type(append) == "function" then
        append(text, role or "agent")
    end
end

return M
