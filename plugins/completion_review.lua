-- Optional tool: runtime collects it only when completion review is enabled.
-- The runtime tool handler owns the completion handshake, never this fallback.
local plugin = {
    id = 'completion_review',
    name = 'Completion Review',
    description = 'Request acceptance of a result through runtime completion review',
}

plugin.tool = {
    name = 'request_completion',
    permission = false,
    description = 'Finish the turn or request a review. Use ready when a requested task is finished; it returns your final answer and never starts a review. Use review ONLY when the user explicitly asks you to review a result or changes; put the review scope in text. Review runs in the background and its findings are reported back to you later. Use question when task completion needs user input, or blocked when work cannot proceed. For ordinary conversation, clarifications and progress updates, reply directly in text, even if you used tools to answer. Tool usage and plain terminal text never trigger review automatically.',
    parameters = {
        type = 'object',
        properties = {
            status = {type = 'string', enum = {'ready', 'review', 'question', 'blocked'}},
            text = {type = 'string', description = 'Final answer (ready), review scope (review), question, or blocker explanation'},
        },
        required = {'status', 'text'},
        additionalProperties = false,
    },
}

function plugin.handler(_ctx)
    local err = 'request_completion is only available through the active runtime completion-review handler.'
    return err, err, false
end

return plugin
