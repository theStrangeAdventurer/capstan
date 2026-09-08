-- Explicit ownership only: never infer a parent from process-global state.
-- Native telemetry is optional and must never affect legacy observers or work.
local M = {}
local active = {}
local shutting_down = false

local function api()
    local capstan = rawget(_G, "capstan")
    return type(capstan) == "table" and type(capstan.telemetry) == "table" and capstan.telemetry or nil
end

local purposes = {agent = true, subagent = true, compaction = true,
    title = true, completion_review = true, empty_response_retry = true}
function M.purpose(value)
    return purposes[value] and value or "agent"
end

function M.start(name, parent, attributes, session_id)
    -- Explicit local correlation only; native start owns log-session capture.
    local span = {children = {}, parent = parent,
        session_id = session_id or (parent and parent.session_id)}
    if shutting_down or (parent and parent.ended) then
        span.ended = true
        return span
    end
    active[span] = true
    if parent and parent.children and not parent.ended then parent.children[span] = true end
    local native = api()
    if native and type(native.start) == "function" then
        local ok, context = pcall(native.start, name,
            parent and (parent.context or (not parent.children and parent)) or nil, attributes)
        if ok and type(context) == "table" then span.context = context end
    end
    return span
end

function M.finish(span, ok, cancelled, attributes)
    if not span or span.ended then return end
    span.ended = true
    active[span] = nil
    -- Unsettled descendants cannot survive a terminal owner (setup exceptions,
    -- observer failure, cancellation). Completed children detach immediately.
    local failure
    local function settle(fn, ...)
        local success, err = pcall(fn, ...)
        if not success and not failure then failure = {err} end
    end
    while next(span.children) do
        settle(M.finish, next(span.children), false, cancelled)
    end
    if span.parent and span.parent.children then span.parent.children[span] = nil end
    local native = api()
    if span.context and native and type(native.end_span) == "function" then
        pcall(native.end_span, span.context, ok == true, cancelled == true, attributes)
    end
    local on_terminal = span.on_terminal
    span.on_terminal = nil
    if on_terminal then settle(on_terminal, ok == true, cancelled == true) end
    if failure then error(failure[1], 0) end
end

-- Copy only known numeric measurements, never arbitrary provider/HTTP data.
function M.measurements(metrics, attributes)
    attributes = attributes or {}
    metrics = type(metrics) == "table" and metrics or {}
    local function numbers(source, keys, prefix)
        if type(source) ~= "table" then return end
        for _, key in ipairs(keys) do
            local value = source[key]
            if type(value) == "number" and value == value and math.abs(value) < math.huge then
                attributes[(prefix or "") .. key] = value
            end
        end
    end
    numbers(metrics, {"first_output_ms", "first_reasoning_ms", "first_text_ms",
        "first_tool_ms", "events", "raw_bytes", "text_chunks", "reasoning_chunks",
        "tool_delta_chunks", "usage_chunks"})
    numbers(metrics.usage, {"prompt_tokens", "completion_tokens", "total_tokens"}, "usage.")
    numbers(metrics.transport, {"http_status", "curl_code", "download_bytes",
        "upload_bytes", "chunk_count", "redirect_count", "ttfb_ms"}, "transport.")
    return attributes
end

-- Close owned work on unexpected exceptions, then preserve the original error.
function M.protect(span, fn, ...)
    local values = table.pack(pcall(fn, ...))
    if not values[1] then
        if span then span.terminal_error = values[2] end
        pcall(M.finish, span, false, false)
        -- Settle async owners too (notably the subagent scheduler). Preserve
        -- the triggering exception even if the terminal observer also fails.
        if span and span.on_exception then pcall(span.on_exception, values[2]) end
        error(values[2], 0)
    end
    return table.unpack(values, 2, values.n)
end

function M.shutdown()
    shutting_down = true
    while next(active) do
        pcall(M.finish, next(active), false, true)
    end
end

return M
