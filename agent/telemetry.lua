-- Explicit ownership only: never infer a parent from process-global state.
-- Native telemetry is optional and must never affect legacy observers or work.
local M = {}
local active = {}
-- Wrapper fields are diagnostic conveniences, not inheritance authority.
local contexts = setmetatable({}, {__mode = "k"})
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
            parent and (contexts[parent] or parent.context or (not parent.children and parent)) or nil, attributes)
        if ok and type(context) == "table" then
            span.context = context
            contexts[span] = context
        end
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
        settle(M.finish, next(span.children), false, cancelled,
            attributes and {["error.category"] = attributes["error.category"]})
    end
    if span.parent and span.parent.children then span.parent.children[span] = nil end
    -- Snapshot owner measurements before native closure, including forced
    -- descendant cancellation. Terminal observers run afterwards as before.
    local finish_attributes = span.finish_attributes
    span.finish_attributes = nil
    if finish_attributes then
        settle(function() attributes = finish_attributes(attributes) end)
    end
    local native = api()
    local context = contexts[span] or span.context
    if context and native and type(native.end_span) == "function" then
        pcall(native.end_span, context, ok == true, cancelled == true, attributes)
    end
    local on_terminal = span.on_terminal
    span.on_terminal = nil
    if on_terminal then settle(on_terminal, ok == true, cancelled == true) end
    if failure then error(failure[1], 0) end
end

-- Only scalar, explicitly selected targets; never serialize argument tables.
-- Native export owns opt-in, redaction and byte bounds for both sinks.
function M.tool_attributes(name, args, attributes)
    attributes = attributes or {}
    args = type(args) == "table" and args or {}
    if name == "shell" then
        if type(args.command) == "string" then attributes["shell.command"] = args.command end
    else
        local targets = {file_read = "path", file_edit = "path", file_write = "path",
            fetch = "url", wiki_read = "path", wiki_write = "path", wiki_source_read = "path"}
        local key = targets[name]
        if key and type(args[key]) == "string" then attributes["tool.target"] = args[key] end
    end
    return attributes
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
    -- Normalized by stream.lua; retain the canonical native attribute names.
    numbers(metrics.usage, {"cached_tokens", "reasoning_tokens"})
    numbers(metrics.transport, {"http_status", "curl_code", "download_bytes",
        "upload_bytes", "chunk_count", "redirect_count", "ttfb_ms",
        "namelookup_elapsed_ms", "connect_elapsed_ms", "appconnect_elapsed_ms",
        "pretransfer_elapsed_ms", "starttransfer_elapsed_ms", "total_ms",
        "dns_ms", "tcp_connect_ms", "tls_handshake_ms", "request_setup_ms",
        "upload_and_server_wait_ms", "download_ms"}, "transport.")
    -- Native HTTP uses past-tense byte counters. Preserve the established OTLP
    -- names; older provider adapters may already supply those names directly.
    if type(metrics.transport) == "table" then
        for target, source in pairs({download_bytes = "downloaded_bytes",
            upload_bytes = "uploaded_bytes"}) do
            local value = metrics.transport[source]
            if type(value) == "number" and value >= 0 and value < math.huge then
                attributes["transport." .. target] = value
            end
        end
    end
    return attributes
end

-- Close owned work on unexpected exceptions, then preserve the original error.
function M.protect(span, fn, ...)
    local values = table.pack(pcall(fn, ...))
    if not values[1] then
        if span then span.terminal_error = values[2] end
        pcall(M.finish, span, false, false, {["error.category"] = "exception"})
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
