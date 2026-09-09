-- Standalone: vendor/lua-5.5.0/src/lua test/test_telemetry.lua
package.path = "./?.lua;./?/init.lua;" .. package.path
local telemetry = require("agent.telemetry")
local original_capstan = rawget(_G, "capstan")
local tests = {}

local function eq(actual, expected, label)
    assert(actual == expected, (label or "value") .. ": expected " ..
        tostring(expected) .. ", got " .. tostring(actual))
end

local function fake_api()
    local starts, ends, counts = {}, {}, {}
    _G.capstan = {telemetry = {
        start = function(name, parent, attributes)
            local context = {id = #starts + 1}
            starts[#starts + 1] = {
                name = name, parent = parent, attributes = attributes, context = context,
            }
            return context
        end,
        end_span = function(context, ok, cancelled, attributes)
            counts[context] = (counts[context] or 0) + 1
            ends[#ends + 1] = {
                context = context, ok = ok, cancelled = cancelled, attributes = attributes,
            }
        end,
    }}
    return starts, ends, counts
end

function tests.explicit_parents_and_native_context()
    local starts, ends, counts = fake_api()
    local external = {trace_id = "test-trace", span_id = "test-parent"}
    local attributes = {operation = "agent", depth = 0}
    local root = telemetry.start("agent.run", external, attributes)
    local model = telemetry.start("agent.model", root)
    local unrelated = telemetry.start("agent.run")
    eq(starts[1].parent, external)
    eq(starts[1].attributes, attributes)
    eq(starts[2].parent, root.context)
    eq(starts[3].parent, nil, "no implicit active parent")
    eq(root.children[model], true)
    eq(root.children[unrelated], nil)
    local measurements = {duration_ms = 7}
    telemetry.finish(model, true, false, measurements)
    telemetry.finish(model, false, true)
    eq(ends[1].attributes, measurements)
    eq(ends[1].ok, true)
    eq(ends[1].cancelled, false)
    eq(counts[model.context], 1)
    eq(next(root.children), nil)
    telemetry.finish(root, true, false)
    eq(unrelated.ended, nil)
    telemetry.finish(unrelated, true, false)
    eq(#ends, 3)
end

function tests.tool_detail_selection()
    local a = telemetry.tool_attributes("shell", {command = "echo fixture", env = {private = true}, stdin = "private"})
    eq(a["shell.command"], "echo fixture")
    eq(a.env, nil)
    eq(a.stdin, nil)
    eq(telemetry.tool_attributes("file_read", {path = "fixture"})["tool.target"], "fixture")
    eq(next(telemetry.tool_attributes("file_read", {paths = {"a", "b"}})), nil)
    eq(next(telemetry.tool_attributes("unknown", {path = "fixture", command = "private"})), nil)
    eq(next(telemetry.tool_attributes("shell", {command = {"private"}})), nil)
end

function tests.owner_closes_descendants_once()
    local _, ends, counts = fake_api()
    local root = telemetry.start("agent.run")
    local tool = telemetry.start("agent.tool", root)
    local child = telemetry.start("agent.run", tool)
    local model = telemetry.start("agent.model", child)
    telemetry.finish(root, false, true)
    local expected = {model, child, tool, root}
    for i, span in ipairs(expected) do
        eq(ends[i].context, span.context, "descendant ends before owner")
        eq(ends[i].ok, false)
        eq(ends[i].cancelled, true)
        eq(span.ended, true)
        eq(next(span.children), nil)
        telemetry.finish(span, true, false)
        eq(counts[span.context], 1)
    end
    telemetry.finish(nil)
    eq(#ends, 4)
end

function tests.interleaved_siblings_keep_ownership()
    local starts, ends, counts = fake_api()
    local root = telemetry.start("agent.run")
    local tool = telemetry.start("agent.tool", root)
    local left = telemetry.start("agent.run", tool)
    local right = telemetry.start("agent.run", tool)
    local left_model = telemetry.start("agent.model", left)
    local right_model = telemetry.start("agent.model", right)
    eq(starts[5].parent, left.context)
    eq(starts[6].parent, right.context)
    telemetry.finish(left_model, true, false)
    telemetry.finish(left, true, false)
    eq(right.ended, nil)
    eq(right_model.ended, nil)
    eq(tool.children[left], nil)
    eq(tool.children[right], true)
    telemetry.finish(tool, false, true)
    eq(counts[left.context], 1)
    eq(counts[left_model.context], 1)
    eq(counts[right.context], 1)
    eq(counts[right_model.context], 1)
    eq(ends[1].ok, true, "completed sibling status preserved")
    eq(ends[2].cancelled, false)
    eq(root.ended, nil)
    telemetry.finish(root, false, true)
    eq(#ends, 6)
end

function tests.completed_spans_detach_immediately()
    local _, ends, counts = fake_api()
    local root = telemetry.start("agent.run")
    for _ = 1, 128 do
        local child = telemetry.start("agent.model", root)
        telemetry.finish(child, true, false)
        eq(next(root.children), nil, "owner does not accumulate completed spans")
        eq(counts[child.context], 1)
    end
    telemetry.finish(root, false, true)
    eq(#ends, 129)
    for i = 1, 128 do
        eq(ends[i].ok, true)
        eq(ends[i].cancelled, false)
    end
end

function tests.measurement_allowlist()
    local keys = {
        "first_output_ms", "first_reasoning_ms", "first_text_ms", "first_tool_ms",
        "events", "raw_bytes", "text_chunks", "reasoning_chunks",
        "tool_delta_chunks", "usage_chunks",
    }
    local usage = {"prompt_tokens", "completion_tokens", "total_tokens"}
    local transport = {"http_status", "curl_code", "download_bytes", "upload_bytes",
        "chunk_count", "redirect_count", "ttfb_ms"}
    local metrics = {usage = {}, transport = {}, text = "fixture text", unknown = 123}
    local expected = {duration_ms = 9}
    local function fill(target, names, prefix)
        for i, name in ipairs(names) do
            target[name] = i - 1
            expected[prefix .. name] = i - 1
        end
    end
    fill(metrics, keys, "")
    fill(metrics.usage, usage, "usage.")
    fill(metrics.transport, transport, "transport.")
    metrics.usage.extra_tokens = 42
    metrics.transport.url = "https://example.invalid/test"
    metrics.transport.headers = {fixture = "not a measurement"}
    local attributes = {duration_ms = 9}
    local result = telemetry.measurements(metrics, attributes)
    eq(result, attributes, "augment caller attributes")
    for key, value in pairs(expected) do eq(result[key], value, key) end
    for key, value in pairs(result) do eq(value, expected[key], "unexpected " .. key) end
    eq(metrics["usage.prompt_tokens"], nil, "source not flattened in place")
    eq(metrics.unknown, 123, "source preserved")
end

function tests.normalized_cache_and_reasoning_measurements()
    local result = telemetry.measurements({usage = {cached_tokens = 12,
        reasoning_tokens = 34, arbitrary = 56}})
    eq(result.cached_tokens, 12)
    eq(result.reasoning_tokens, 34)
    eq(result.arbitrary, nil)
    for _, invalid in ipairs({"12", false, {}, 0/0, math.huge, -math.huge}) do
        eq(next(telemetry.measurements({usage = {cached_tokens = invalid,
            reasoning_tokens = invalid}})), nil)
    end
end

function tests.native_http_phases_and_byte_names()
    local phases = {"namelookup_elapsed_ms", "connect_elapsed_ms",
        "appconnect_elapsed_ms", "pretransfer_elapsed_ms", "starttransfer_elapsed_ms",
        "total_ms", "dns_ms", "tcp_connect_ms", "tls_handshake_ms",
        "request_setup_ms", "upload_and_server_wait_ms", "download_ms"}
    local transport = {uploaded_bytes = 123, downloaded_bytes = 456}
    for i, key in ipairs(phases) do transport[key] = i / 2 end
    local result = telemetry.measurements({transport = transport})
    eq(result["transport.upload_bytes"], 123)
    eq(result["transport.download_bytes"], 456)
    for i, key in ipairs(phases) do eq(result["transport." .. key], i / 2) end
    eq(result["transport.uploaded_bytes"], nil, "one canonical byte name")
    eq(result["transport.downloaded_bytes"], nil)
    eq(telemetry.measurements({transport = {redirect_count = 1}})["transport.dns_ms"],
        nil, "missing redirect phases stay absent")
    for _, value in ipairs({"123", false, {}, 0/0, math.huge, -1}) do
        eq(next(telemetry.measurements({transport = {uploaded_bytes = value,
            downloaded_bytes = value}})), nil)
    end
end

function tests.invalid_measurements_are_ignored()
    for _, invalid in ipairs({"12", true, {}, 0/0, math.huge, -math.huge}) do
        local result = telemetry.measurements({events = invalid,
            usage = {total_tokens = invalid}, transport = {ttfb_ms = invalid}})
        eq(next(result), nil)
    end
    for _, invalid in ipairs({false, "invalid", 12}) do
        eq(next(telemetry.measurements(invalid)), nil)
        eq(next(telemetry.measurements({usage = invalid, transport = invalid})), nil)
    end
    eq(next(telemetry.measurements(nil)), nil)
    eq(telemetry.measurements({first_text_ms = 0.25}).first_text_ms, 0.25)
end

function tests.optional_api_and_start_failures()
    local variants = {false, {}, {telemetry = false}, {telemetry = {}},
        {telemetry = {start = false, end_span = false}},
        {telemetry = {start = function() error("exporter unavailable") end}},
        {telemetry = {start = function() return "invalid context" end}}}
    -- Include a genuinely absent global as well as malformed/partial APIs.
    variants[#variants + 1] = {absent = true}
    for _, variant in ipairs(variants) do
        _G.capstan = variant
        if type(variant) == "table" and variant.absent then _G.capstan = nil end
        local root = telemetry.start("agent.run")
        local child = telemetry.start("agent.model", root)
        eq(root.context, nil)
        telemetry.finish(root, true, false)
        eq(root.ended, true)
        eq(child.ended, true)
        eq(next(root.children), nil)
    end
    fake_api()
    local root = telemetry.start("agent.run")
    _G.capstan.telemetry = nil
    telemetry.finish(root, true, false)
    eq(root.ended, true)
end

function tests.failed_export_is_best_effort_and_not_retried()
    fake_api()
    local calls = 0
    _G.capstan.telemetry.end_span = function()
        calls = calls + 1
        error("export failed")
    end
    local root = telemetry.start("agent.run")
    local child = telemetry.start("agent.tool", root)
    telemetry.finish(root, true, false)
    telemetry.finish(root, false, true)
    telemetry.finish(child, false, true)
    eq(calls, 2)
    eq(next(root.children), nil)
    eq(child.ended, true)
end

function tests.missing_native_parent_does_not_become_context()
    _G.capstan = nil
    local root = telemetry.start("agent.run")
    local starts = fake_api()
    local child = telemetry.start("agent.model", root)
    eq(starts[1].parent, nil, "Lua ownership wrapper is not a native context")
    telemetry.finish(root, false, false)
    eq(child.ended, true)
end

function tests.protect_preserves_values_and_original_exception()
    local _, ends = fake_api()
    local root = telemetry.start("agent.run")
    local values = table.pack(telemetry.protect(root, function(...)
        eq(select("#", ...), 3)
        local a, b, c = ...
        eq(a, "input"); eq(b, nil); eq(c, false)
        return nil, "result", false, nil
    end, "input", nil, false))
    eq(values.n, 4)
    eq(values[1], nil); eq(values[2], "result"); eq(values[3], false); eq(values[4], nil)
    eq(root.ended, nil, "successful protect leaves asynchronous work open")
    local child = telemetry.start("agent.model", root)
    local original_error = {message = "observer exception"}
    local ok, err = pcall(telemetry.protect, root, function() error(original_error) end)
    eq(ok, false)
    eq(err, original_error, "exception identity preserved")
    eq(ends[1].attributes["error.category"], "exception")
    eq(ends[2].attributes["error.category"], "exception")
    eq(root.ended, true); eq(child.ended, true)
    eq(#ends, 2)
    eq(ends[1].ok, false); eq(ends[1].cancelled, false)
    eq(ends[2].ok, false); eq(ends[2].cancelled, false)
    telemetry.finish(root, true, false)
    eq(#ends, 2)

    fake_api()
    root = telemetry.start("agent.run")
    _G.capstan.telemetry.end_span = function() error("secondary exporter error") end
    ok, err = pcall(telemetry.protect, root, function() error(original_error) end)
    eq(ok, false); eq(err, original_error)
end

function tests.purpose_allowlist_and_strict_status_booleans()
    for _, purpose in ipairs({"agent", "subagent", "compaction", "title",
        "completion_review", "empty_response_retry"}) do
        eq(telemetry.purpose(purpose), purpose)
    end
    eq(telemetry.purpose(nil), "agent")
    for _, purpose in ipairs({"custom task text", "", false, 12, {}}) do
        eq(telemetry.purpose(purpose), "agent")
    end
    local _, ends = fake_api()
    telemetry.finish(telemetry.start("agent.run"), 1, "cancelled")
    eq(ends[1].ok, false)
    eq(ends[1].cancelled, false)
end

function tests.parent_terminal_settles_callbacks_despite_native_failure()
    local _, _, counts = fake_api()
    local root = telemetry.start("agent.run")
    local left = telemetry.start("agent.run", root)
    local right = telemetry.start("agent.run", root)
    local model = telemetry.start("agent.model", left)
    local calls, exports = {}, 0
    capstan.telemetry.end_span = function()
        exports = exports + 1
        error("native unavailable")
    end
    for _, span in ipairs({root, left, right, model}) do
        span.on_terminal = function(ok, cancelled)
            eq(ok, false); eq(cancelled, true)
            eq(span.ended, true)
            eq(next(span.children), nil)
            calls[span] = (calls[span] or 0) + 1
            telemetry.finish(span, true, false)
        end
    end
    telemetry.finish(root, false, true)
    for _, span in ipairs({root, left, right, model}) do
        telemetry.finish(span, true, false)
        eq(calls[span], 1)
        eq(span.on_terminal, nil)
    end
    eq(exports, 4)
    eq(next(counts), nil)
end

function tests.shutdown_cancels_all_once_and_blocks_native_starts()
    -- A fresh instance leaves the shared module usable by all other tests.
    local fresh = assert(loadfile('agent/telemetry.lua'))()
    local starts, ends, counts = fake_api()
    local root = fresh.start('agent.run')
    local child = fresh.start('agent.model', root)
    local unrelated = fresh.start('agent.run')
    local completed = fresh.start('agent.tool')
    fresh.finish(completed, true, false)
    local calls = {}
    for _, span in ipairs({root, child, unrelated}) do
        span.on_terminal = function(ok, cancelled)
            calls[span] = (calls[span] or 0) + 1
            eq(ok, false); eq(cancelled, true)
            eq(span.ended, true)
            eq(next(span.children), nil)
            fresh.finish(span, true, false)
            local blocked = fresh.start('title')
            eq(blocked.ended, true); eq(blocked.context, nil)
        end
    end
    fresh.shutdown()
    fresh.shutdown()
    for _, span in ipairs({root, child, unrelated}) do
        fresh.finish(span, true, false)
        eq(calls[span], 1)
        eq(counts[span.context], 1)
        eq(span.on_terminal, nil)
    end
    for _, parent in ipairs({false, root}) do
        local blocked = fresh.start('agent.run', parent or nil)
        eq(blocked.ended, true); eq(blocked.context, nil)
        fresh.finish(blocked, true, false)
    end
    eq(#starts, 4); eq(#ends, 4)
    eq(counts[completed.context], 1)
    eq(ends[1].ok, true); eq(ends[1].cancelled, false)
    for i = 2, #ends do
        eq(ends[i].ok, false); eq(ends[i].cancelled, true)
    end
    eq(require('agent.telemetry'), telemetry)
end

function tests.terminal_observer_exception_still_settles_siblings()
    local _, ends = fake_api()
    local root = telemetry.start("agent.run")
    local left = telemetry.start("agent.run", root)
    local right = telemetry.start("agent.run", root)
    local failure = {}
    local calls = 0
    left.on_terminal = function() error(failure) end
    right.on_terminal = function() calls = calls + 1 end
    root.on_terminal = function() calls = calls + 1 end
    local ok, err = pcall(telemetry.finish, root, false, true)
    eq(ok, false); eq(err, failure)
    eq(calls, 2); eq(#ends, 3)
    eq(next(root.children), nil)
    telemetry.finish(root, false, true)
    eq(#ends, 3)
end

-- Invoked inside the existing C provider fixture, with real runtime and mocked
-- transport. Standalone helper tests above deliberately need no native runtime.
if rawget(_G, "TELEMETRY_RUNTIME_FIXTURE") then
    local runtime = require("agent.runtime")
    local native_capstan = capstan
    local starts, ends, counts = fake_api()
    native_capstan.telemetry = capstan.telemetry
    capstan = native_capstan
    local streams, results, model_done = {}, {}, 0
    local original_now_ms, clock = capstan.now_ms, 0
    capstan.now_ms = function()
        clock = clock + 7
        return clock
    end
    http.post_stream = function(_, _, _, callback)
        streams[#streams + 1] = callback
        return #streams
    end
    local function run(options)
        options = options or {}
        options.messages = {{role = "user", content = "fixture request"}}
        return runtime.run(options, {
            on_run_start = function(context)
                eq(context, starts[#starts].context, "root context delivered before model start")
                eq(starts[#starts].name, (tonumber(options.depth) or 0) > 0 and "subagent" or "agent.run")
                eq(counts[context], nil, "run is still active")
            end,
            on_model_done = function(_, _, _, duration_ms)
                model_done = model_done + 1
                eq(duration_ms, ends[#ends].attributes.duration_ms,
                    "native and legacy model duration are identical")
            end,
            on_done = function(result, context)
                results[#results + 1] = {result = result, context = context}
            end,
        })
    end
    local function complete(callback)
        callback('data: {"choices":[{"delta":{"content":"fixture answer"}}]}\n\n', false)
        callback(nil, true)
    end
    local function paired(first, operation)
        eq(starts[first].name, operation == "subagent" and "subagent" or "agent.run")
        eq(starts[first + 1].name, "agent.model")
        eq(starts[first + 1].parent, starts[first].context)
        eq(starts[first].attributes.operation, operation)
        eq(starts[first + 1].attributes.operation, operation)
        eq(counts[starts[first].context], 1)
        eq(counts[starts[first + 1].context], 1)
    end
    assert(run())
    eq(#starts, 2); eq(#ends, 0)
    complete(streams[1])
    paired(1, "agent")
    eq(ends[1].context, starts[2].context)
    eq(ends[2].context, starts[1].context)
    eq(ends[1].ok, true); eq(ends[2].ok, true)
    eq(results[1].context, starts[1].context)
    eq(results[1].result.ok, true)
    complete(streams[1])
    eq(#ends, 2); eq(#results, 1); eq(model_done, 1)

    local before = #starts
    local ok, err = run({provider = "missing-fixture-provider"})
    assert(not ok and err)
    eq(#starts, before + 1); eq(#streams, 1)
    eq(ends[#ends].ok, false)
    eq(counts[starts[#starts].context], 1)
    local hooks = require("agent.hooks")
    assert(hooks.register("before_request", function(ctx)
        ctx.error = "fixture request rejected"
        return ctx
    end, {source = "telemetry-fixture"}))
    run()
    eq(#starts, before + 2); eq(#streams, 1)
    eq(results[#results].result.error, "fixture request rejected")
    eq(ends[#ends].ok, false)
    eq(counts[starts[#starts].context], 1)
    hooks.remove_source("telemetry-fixture")

    -- Sibling runs carry explicit tool ownership, never an active-global parent.
    local owner = telemetry.start("agent.tool")
    local first = #starts + 1
    assert(run({depth = 1, purpose = "subagent", telemetry_parent = owner}))
    assert(run({depth = 1, purpose = "subagent", telemetry_parent = owner}))
    eq(starts[first].parent, owner.context)
    eq(starts[first + 2].parent, owner.context)
    local left, right = streams[2], streams[3]
    complete(left)
    paired(first, "subagent")
    eq(counts[starts[first + 2].context], nil)
    local delivered = #results
    telemetry.finish(owner, false, true)
    paired(first + 2, "subagent")
    eq(#results, delivered + 1)
    eq(results[#results].result.cancelled, true)
    eq(results[#results].context, starts[first + 2].context)
    eq(ends[#ends].context, owner.context)
    complete(right); complete(left)
    telemetry.finish(owner, false, true)
    eq(#results, delivered + 1)
    for _, start in ipairs(starts) do eq(counts[start.context], 1) end

    -- A transport retry is another model span under the same subagent run.
    owner = telemetry.start("agent.tool")
    first = #starts + 1
    assert(run({depth = 1, purpose = "subagent", telemetry_parent = owner}))
    local failed_attempt = streams[#streams]
    delivered = #results
    failed_attempt(nil, true, "connection error: fixture retry")
    eq(#starts, first + 3)
    eq(#results, delivered)
    eq(starts[first].parent, owner.context)
    eq(starts[first + 1].attributes.attempt, 1)
    eq(starts[first + 2].attributes.operation, "retry")
    eq(starts[first + 2].attributes.purpose, "stream_transient_error")
    eq(starts[first + 2].parent, starts[first].context)
    eq(starts[first + 3].attributes.attempt, 2)
    eq(starts[first + 3].parent, starts[first].context)
    eq(starts[first + 3].attributes.operation, "subagent")
    eq(ends[#ends - 1].context, starts[first + 1].context)
    eq(ends[#ends - 1].ok, false)
    eq(ends[#ends].context, starts[first + 2].context)
    eq(ends[#ends].ok, true)
    assert(ends[#ends].attributes.duration_ms >= 0)
    complete(failed_attempt)
    eq(#results, delivered)
    complete(streams[#streams])
    paired(first, "subagent")
    eq(counts[starts[first + 2].context], 1)
    eq(#results, delivered + 1)
    eq(results[#results].result.ok, true)
    telemetry.finish(owner, true, false)

    for _, purpose in ipairs({"compaction", "title"}) do
        first = #starts + 1
        assert(run({purpose = purpose}))
        complete(streams[#streams])
        paired(first, purpose)
    end
    local tool_observed = false
    require("agent.tools").handle_tool_calls({}, {}, {
        {id = "missing-tool", name = "missing_fixture_tool", arguments = "{}"},
    }, "", function() end, {silent_tools = true, callbacks = {
        on_tool_done = function(_, _, _, duration_ms)
            tool_observed = true
            eq(ends[#ends].attributes.duration_ms, duration_ms,
                "native and legacy tool duration are identical")
        end,
    }})
    eq(tool_observed, true)

    local settled_failure
    local stream_count = #streams
    local observer_ok, observer_error = pcall(runtime.run, {}, {
        on_run_start = function(context)
            eq(context, starts[#starts].context)
            error("fixture run start failure")
        end,
        on_done = function(result) settled_failure = result end,
    })
    eq(observer_ok, false)
    assert(tostring(observer_error):find("fixture run start failure", 1, true))
    eq(settled_failure.ok, false)
    eq(#streams, stream_count, "failed start observer prevents transport")
    eq(counts[starts[#starts].context], 1)

    local exports = 0
    local record_end = capstan.telemetry.end_span
    capstan.telemetry.end_span = function(...)
        exports = exports + 1
        record_end(...)
        error("fixture native failure")
    end
    delivered = #results
    assert(run())
    complete(streams[#streams])
    eq(exports, 2); eq(#results, delivered + 1)
    eq(results[#results].result.ok, true)
    capstan.now_ms = original_now_ms
    return
end

local names = {}
for name in pairs(tests) do names[#names + 1] = name end
table.sort(names)
local failed = 0
for _, name in ipairs(names) do
    local ok, err = pcall(tests[name])
    _G.capstan = original_capstan
    if ok then
        io.write("PASS ", name, "\n")
    else
        failed = failed + 1
        io.stderr:write("FAIL ", name, ": ", tostring(err), "\n")
    end
end
io.write(string.format("%d tests, %d failures\n", #names, failed))
if failed > 0 then os.exit(1) end
