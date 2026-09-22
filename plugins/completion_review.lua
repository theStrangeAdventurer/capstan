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
    description = 'Request completion, not unconditional acceptance. You MUST call this with ready when a requested task is finished (including after review repairs), or when the user explicitly asks to review a result or changes; put the result or requested review scope in text. This starts independent review even with no edits or prior tools. Use question when task completion needs user input, or blocked when work cannot proceed. For ordinary conversation, clarifications and progress updates, reply directly in text, even if you used tools to answer. Tool usage and plain terminal text never trigger review automatically.',
    parameters = {
        type = 'object',
        properties = {
            status = {type = 'string', enum = {'ready', 'question', 'blocked'}},
            text = {type = 'string', description = 'Proposed final answer, question, or explanation of the blocker'},
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
