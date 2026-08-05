local json = require("vendor.rxi.json")
local images = require("agent.images")
local mcp_client = require("agent.mcp")

local sessions = {}
local next_session = 1
local active = nil
local initialized = false
local pending_responses = {}
local next_server_request = 1000000

local function array(values)
    return json.array(values or {})
end

local function send(value)
    acp.send(json.encode(value))
end

local function response(id, result)
    if id == nil then return end
    send({jsonrpc = "2.0", id = id, result = result or {}})
end

local function rpc_error(id, code, message)
    if id == nil then return end
    send({jsonrpc = "2.0", id = id, error = {code = code, message = message}})
end

local function rpc_error_null(code, message)
    acp.send('{"jsonrpc":"2.0","id":null,"error":{"code":' ..
        tostring(code) .. ',"message":' .. json.encode(message) .. '}}')
end

local function notify(session_id, update)
    send({
        jsonrpc = "2.0",
        method = "session/update",
        params = {sessionId = session_id, update = update},
    })
end

local function find_session(params, id)
    local session_id = type(params) == "table" and params.sessionId or nil
    local session = session_id and sessions[session_id] or nil
    if not session then
        rpc_error(id, -32602, "unknown sessionId")
        return nil
    end
    return session
end

local function command_update(session)
    notify(session.id, {
        sessionUpdate = "available_commands_update",
        availableCommands = array(acp.commands()),
    })
end

local function model_snapshot(profile)
    local effective = capstan.models and capstan.models.effective and
        capstan.models.effective(profile) or nil
    local current_provider = effective and effective.provider or
        (capstan.models and capstan.models.current_provider and
            capstan.models.current_provider() or "")
    local current_model = effective and effective.model or
        (capstan.models and capstan.models.current_model and
            capstan.models.current_model() or "")
    local options = {}
    local known = {}
    if capstan.models and type(capstan.models.configured) == "function" then
        local ok, models = pcall(capstan.models.configured)
        if ok and type(models) == "table" then
            for _, model in ipairs(models) do
                if type(model.provider) == "string" and type(model.id) == "string" then
                    local value = model.provider .. "/" .. model.id
                    known[value] = true
                    table.insert(options, {value = value, name = value})
                end
            end
        end
    end
    local current = current_provider ~= "" and current_model ~= "" and
        (current_provider .. "/" .. current_model) or ""
    if current ~= "" and not known[current] then
        table.insert(options, {value = current, name = current})
    end
    return current_provider, current_model, options
end

local function mode_state(session)
    return {
        currentModeId = session.profile,
        availableModes = array({
            {id = "fast", name = "Fast", description = "Fast answers with a reduced tool set"},
            {id = "implement", name = "Implement", description = "Implement and verify changes"},
            {id = "plan", name = "Plan", description = "Analyze and propose a plan without editing"},
        }),
    }
end

local function config_options(session)
    local options = {}
    if session.model_options and #session.model_options > 0 and session.provider and session.model then
        table.insert(options, {
            id = "model",
            name = "Model",
            category = "model",
            type = "select",
            currentValue = session.provider .. "/" .. session.model,
            options = array(session.model_options),
        })
    end
    table.insert(options, {
        id = "mode",
        name = "Mode",
        description = "Capstan agent profile",
        category = "mode",
        type = "select",
        currentValue = session.profile,
        options = array({
            {value = "fast", name = "Fast"},
            {value = "implement", name = "Implement"},
            {value = "plan", name = "Plan"},
        }),
    })
    table.insert(options, {
        id = "effort",
        name = "Reasoning effort",
        description = "Reasoning effort passed to the active provider",
        category = "thought_level",
        type = "select",
        currentValue = session.effort,
        options = array({
            {value = "none", name = "None"},
            {value = "minimal", name = "Minimal"},
            {value = "low", name = "Low"},
            {value = "medium", name = "Medium"},
            {value = "high", name = "High"},
            {value = "xhigh", name = "Extra high"},
            {value = "max", name = "Maximum"},
        }),
    })
    return array(options)
end

local function prompt_content(blocks)
    if type(blocks) ~= "table" then return nil, nil, "prompt must be an array" end
    local content = {}
    local text_parts = {}
    local has_image = false
    for _, block in ipairs(blocks) do
        if type(block) == "table" and block.type == "text" and type(block.text) == "string" then
            table.insert(content, {type = "text", text = block.text})
            table.insert(text_parts, block.text)
        elseif type(block) == "table" and block.type == "resource" and
            type(block.resource) == "table" and type(block.resource.text) == "string" then
            table.insert(content, {type = "text", text = block.resource.text})
            table.insert(text_parts, block.resource.text)
        elseif type(block) == "table" and block.type == "image" then
            local image, image_err = images.from_mcp(block)
            if not image then
                if image_err == "too_large" then
                    return nil, nil, "image exceeds the 10 MiB decoded size limit"
                end
                return nil, nil, "image must be valid base64 PNG, JPEG, GIF, or WebP data matching its MIME type"
            end
            table.insert(content, {
                type = "image_url",
                image_url = {
                    url = "data:" .. image.mime_type .. ";base64," .. image.data,
                    detail = "auto",
                },
            })
            has_image = true
        else
            return nil, nil, "Capstan ACP accepts text, images, and embedded text resources"
        end
    end
    if #content == 0 then return nil, nil, "prompt must not be empty" end
    return content, table.concat(text_parts, "\n"), nil, has_image
end

local function ensure_mcp_initialized(run)
    mcp_client.ensure_initialized()
    local scope_id = run.session.id
    local started_at = capstan.now_ms()
    while not mcp_client.is_initialized() or
          not mcp_client.is_scope_initialized(scope_id) do
        mcp_client.tick(8)
        mcp_client.tick_scope(scope_id)
        if http and type(http.poll) == "function" then http.poll() end
        if acp and type(acp.pump) == "function" then acp.pump() end
        if active ~= run or run.finished then return false, "cancelled" end
        if mcp_client.is_initialized() and
           mcp_client.is_scope_initialized(scope_id) then break end
        if capstan.now_ms() - started_at >= 30000 then
            return false, "timed out waiting for MCP servers"
        end
        if http and type(http.wait_frame) == "function" then http.wait_frame() end
    end
    return true
end

local function sync_session_runtime(session)
    local previous_profile = session.profile
    if capstan.agent and type(capstan.agent.get_profile) == "function" then
        local ok, profile = pcall(capstan.agent.get_profile)
        if ok and type(profile) == "string" and profile ~= "" then
            session.profile = profile
        end
    end
    local effective = capstan.models and capstan.models.effective and
        capstan.models.effective(session.profile) or nil
    if type(effective) == "table" and type(effective.provider) == "string" and
        type(effective.model) == "string" then
        session.provider = effective.provider
        session.model = effective.model
    end
    if session.profile ~= previous_profile then
        notify(session.id, {
            sessionUpdate = "current_mode_update",
            modeId = session.profile,
        })
    end
end

local function parse_command(text)
    local token = text:match("^%s*(/%S+)")
    if not token then return nil end
    for _, plugin in pairs(plugins or {}) do
        if plugin.command == token and type(plugin.handler) == "function" then
            local args = {}
            local rest = text:match("^%s*/%S+%s*(.*)$") or ""
            for arg in rest:gmatch("%S+") do table.insert(args, arg) end
            local ctx = {input = text, command = token, args = args}
            function ctx:replace(ui, llm)
                return ui, llm == nil and ui or llm
            end
            local ok, ui, llm = pcall(plugin.handler, ctx)
            if not ok then return {error = tostring(ui)} end
            return {
                ui = tostring(ui or ""),
                llm = tostring(llm == nil and ui or llm or ""),
                history = plugin.history ~= false,
            }
        end
    end
    return nil
end

local function tool_arguments(tool_call)
    if type(tool_call.arguments) == "table" then return tool_call.arguments end
    if type(tool_call.arguments) == "string" then
        local ok, decoded = pcall(json.decode, tool_call.arguments)
        if ok and type(decoded) == "table" then return decoded end
    end
    return {}
end

local function tool_kind(name)
    name = tostring(name or ""):lower()
    if name == "shell" then return "execute" end
    if name == "file_read" or name == "wiki_read" or name == "wiki_source_read" then return "read" end
    if name == "file_write" or name == "file_edit" or name == "wiki_ingest" then return "edit" end
    if name == "fetch" or name:find("browser_navigate", 1, true) then return "fetch" end
    if name == "subagents" then return "think" end
    if name:find("find", 1, true) or name:find("search", 1, true) then return "search" end
    return "other"
end

local function tool_locations(name, args, cwd)
    local path = args.path or args.filePath or args.filepath
    if type(path) == "string" and path ~= "" then return array({{path = path}}) end
    if name == "shell" and cwd then return array({{path = cwd}}) end
    return array({})
end

local function permission_request(session, permission_tool, target, details)
    details = type(details) == "table" and details or {}
    local name = tostring(details.tool_name or permission_tool or "tool")
    local args = type(details.arguments) == "table" and details.arguments or {}
    local request_id = next_server_request
    next_server_request = next_server_request + 1
    send({
        jsonrpc = "2.0",
        id = request_id,
        method = "session/request_permission",
        params = {
            sessionId = session.id,
            toolCall = {
                toolCallId = tostring(details.tool_call_id or ("permission-" .. request_id)),
                title = tostring(target or name),
                kind = tool_kind(name),
                status = "pending",
                locations = tool_locations(name, args, session.cwd),
                rawInput = args,
            },
            options = array({
                {optionId = "allow-once", name = "Allow once", kind = "allow_once"},
                {optionId = "allow-always", name = "Always allow", kind = "allow_always"},
                {optionId = "reject-once", name = "Reject", kind = "reject_once"},
            }),
        },
    })
    local selected = acp.wait_response(request_id)
    if selected == "allow-once" then return "allow" end
    if selected == "allow-always" then return "allow_session" end
    return "deny"
end

local function finish_active(stop_reason, result)
    local run = active
    if not run or run.finished then return end
    run.finished = true
    active = nil
    local session = run.session
    if result and result.ok and type(result.text) == "string" and result.text ~= "" then
        table.insert(session.messages, {role = "assistant", content = result.text})
    end
    response(run.request_id, {stopReason = stop_reason})
end

local function start_prompt(id, params)
    local session = find_session(params, id)
    if not session then return end
    if active then
        rpc_error(id, -32000, "another prompt is already running")
        return
    end

    local content, text, err, has_image = prompt_content(params.prompt)
    if not content then
        rpc_error(id, -32602, err)
        return
    end
    if text == "" and not has_image then
        rpc_error(id, -32602, "prompt must not be empty")
        return
    end

    local ok, dir_err = acp.set_directory(session.cwd)
    if not ok then
        rpc_error(id, -32602, dir_err)
        return
    end

    local command = not has_image and parse_command(text) or nil
    if command and command.error then
        rpc_error(id, -32000, command.error)
        return
    elseif command then
        sync_session_runtime(session)
        if command.ui ~= "" then
            notify(session.id, {
                sessionUpdate = "agent_message_chunk",
                content = {type = "text", text = command.ui},
            })
        end
        if not command.history then
            response(id, {stopReason = "end_turn"})
            return
        end
        text = command.llm
        content = text
    elseif not has_image then
        content = text
    end

    local run = {request_id = id, session = session, finished = false}
    active = run
    local mcp_ok, mcp_err = ensure_mcp_initialized(run)
    if not mcp_ok then
        if active ~= run or run.finished then return end
        active = nil
        rpc_error(id, -32000, mcp_err)
        return
    end

    table.insert(session.messages, {role = "user", content = content})
    local started, start_err = capstan.agent.run({
        messages = session.messages,
        profile = session.profile,
        provider = session.provider,
        model = session.model,
        reasoning_effort = session.effort,
        max_turns = 200,
        silent_tools = true,
        permission_scope = session.permission_scope,
        mcp_scope = session.id,
        is_cancelled = function()
            return active ~= run or run.finished
        end,
    }, {
        on_text = function(chunk)
            if active ~= run or run.finished then return end
            notify(session.id, {
                sessionUpdate = "agent_message_chunk",
                content = {type = "text", text = chunk},
            })
        end,
        on_tool_start = function(tool_call)
            if active ~= run or run.finished then return end
            local args = tool_arguments(tool_call)
            notify(session.id, {
                sessionUpdate = "tool_call",
                toolCallId = tostring(tool_call.id or ""),
                title = tostring(args.command or args.path or tool_call.name or "tool"),
                kind = tool_kind(tool_call.name),
                status = "pending",
                locations = tool_locations(tool_call.name, args, session.cwd),
                rawInput = args,
            })
        end,
        on_tool_done = function(tool_call, tool_result, tool_ok)
            if active ~= run or run.finished then return end
            local text = tostring(tool_result or "")
            notify(session.id, {
                sessionUpdate = "tool_call_update",
                toolCallId = tostring(tool_call.id or ""),
                status = tool_ok and "completed" or "failed",
                content = array({{type = "content", content = {
                    type = "text", text = text,
                }}}),
                rawOutput = tool_ok and {output = text} or {error = text},
            })
        end,
        on_permission_request = function(permission_tool, target, details)
            if active ~= run or run.finished then return "deny" end
            return permission_request(session, permission_tool, target, details)
        end,
        on_error = function(message)
            if active ~= run or run.finished then return end
            notify(session.id, {
                sessionUpdate = "agent_message_chunk",
                content = {type = "text", text = "\n[error: " .. tostring(message) .. "]\n"},
            })
        end,
        on_done = function(result)
            if active ~= run or run.finished then return end
            finish_active(result and result.ok == false and "refusal" or "end_turn", result)
        end,
    })

    if not started and active == run and not run.finished then
        table.remove(session.messages)
        run.finished = true
        active = nil
        rpc_error(id, -32000, tostring(start_err or "could not start agent run"))
    end
end

local handlers = {}

handlers.initialize = function(id, params)
    if initialized then
        rpc_error(id, -32600, "initialize called more than once")
        return
    end
    local requested = params and params.protocolVersion
    if type(requested) ~= "number" or requested ~= 1 then
        rpc_error(id, -32602, "protocolVersion must be ACP version 1")
        return
    end
    initialized = true
    response(id, {
        protocolVersion = 1,
        agentCapabilities = {
            loadSession = false,
            promptCapabilities = {image = true, audio = false, embeddedContext = true},
            mcpCapabilities = {http = true, sse = false},
            sessionCapabilities = {close = {}},
        },
        agentInfo = {name = "capstan", title = "Capstan", version = tostring(acp.version or "local")},
        authMethods = array({}),
    })
end

handlers["session/new"] = function(id, params)
    if not initialized then
        rpc_error(id, -32002, "initialize must be called first")
        return
    end
    if active then
        rpc_error(id, -32000, "cannot create a session while a prompt is running")
        return
    end
    local mcp_servers = type(params) == "table" and params.mcpServers or nil
    if type(mcp_servers) ~= "table" then
        rpc_error(id, -32602, "mcpServers must be an array")
        return
    end
    local cwd = type(params) == "table" and params.cwd or nil
    if type(cwd) ~= "string" or cwd:sub(1, 1) ~= "/" then
        rpc_error(id, -32602, "cwd must be an absolute path")
        return
    end
    local ok, err = acp.set_directory(cwd)
    if not ok then
        rpc_error(id, -32602, err)
        return
    end
    local session_id = string.format("capstan-%d-%d", os.time(), next_session)
    next_session = next_session + 1
    local attached, attach_err = mcp_client.attach_scope(session_id, mcp_servers)
    if not attached then
        rpc_error(id, -32602, attach_err)
        return
    end
    local profile = capstan.agent and capstan.agent.get_profile and
        capstan.agent.get_profile() or "implement"
    local provider, model, model_options = model_snapshot(profile)
    local session = {
        id = session_id,
        cwd = cwd,
        messages = {},
        profile = profile,
        provider = provider ~= "" and provider or nil,
        model = model ~= "" and model or nil,
        model_options = model_options,
        effort = "high",
        permission_scope = {allowed_tools = {}, allowed_targets = {}, full_control = false},
    }
    sessions[session_id] = session
    response(id, {
        sessionId = session_id,
        configOptions = config_options(session),
        modes = mode_state(session),
    })
    command_update(session)
end

handlers["session/prompt"] = start_prompt

handlers["session/cancel"] = function(id, params)
    local session = find_session(params, id)
    if not session then return end
    if active and active.session == session and not active.finished then
        acp.cancel()
        finish_active("cancelled", {ok = false})
    end
    if id ~= nil then response(id, {}) end
end

handlers["session/set_config_option"] = function(id, params)
    local session = find_session(params, id)
    if not session then return end
    local config_id = params.configId
    local value = params.value
    if config_id == "mode" and (value == "fast" or value == "implement" or value == "plan") then
        session.profile = value
        notify(session.id, {sessionUpdate = "current_mode_update", modeId = value})
    elseif config_id == "model" and type(value) == "string" then
        local valid = false
        for _, option in ipairs(session.model_options or {}) do
            if option.value == value then valid = true break end
        end
        local provider, model = value:match("^([^/]+)/(.+)$")
        if not valid or not provider or not model then
            rpc_error(id, -32602, "invalid model")
            return
        end
        session.provider = provider
        session.model = model
    elseif config_id == "effort" and
        (value == "none" or value == "minimal" or value == "low" or value == "medium" or
         value == "high" or value == "xhigh" or value == "max") then
        session.effort = value
    else
        rpc_error(id, -32602, "invalid config option")
        return
    end
    response(id, {configOptions = config_options(session)})
end

handlers["session/set_mode"] = function(id, params)
    local session = find_session(params, id)
    if not session then return end
    local mode = params.modeId
    if mode ~= "fast" and mode ~= "implement" and mode ~= "plan" then
        rpc_error(id, -32602, "invalid mode")
        return
    end
    session.profile = mode
    notify(session.id, {sessionUpdate = "current_mode_update", modeId = mode})
    response(id, {})
end

handlers["session/close"] = function(id, params)
    local session = find_session(params, id)
    if not session then return end
    if active and active.session == session then
        acp.cancel()
        finish_active("cancelled", {ok = false})
    end
    mcp_client.close_scope(session.id)
    sessions[session.id] = nil
    response(id, {})
end

function capstan_acp_handle(line)
    local ok, request = pcall(json.decode, line)
    if not ok or type(request) ~= "table" then
        rpc_error_null(-32700, "parse error")
        return
    end
    if request.jsonrpc == "2.0" and request.id ~= nil and request.method == nil and
        (request.result ~= nil or request.error ~= nil) then
        pending_responses[tostring(request.id)] = request
        return
    end
    if request.jsonrpc ~= "2.0" or type(request.method) ~= "string" then
        if request.id == nil then
            rpc_error_null(-32600, "invalid request")
        else
            rpc_error(request.id, -32600, "invalid request")
        end
        return
    end
    if request.id == nil and request.method ~= "session/cancel" then
        return
    end
    local handler = handlers[request.method]
    if not handler then
        if request.id ~= nil then rpc_error(request.id, -32601, "method not found") end
        return
    end
    local handled, err = pcall(handler, request.id, request.params or {})
    if not handled and request.id ~= nil then
        rpc_error(request.id, -32603, tostring(err))
    end
end

function capstan_acp_take_response(request_id)
    local key = tostring(request_id)
    local message = pending_responses[key]
    if not message then return nil end
    pending_responses[key] = nil
    local outcome = message.result and message.result.outcome
    if type(outcome) == "table" and outcome.outcome == "selected" then
        return tostring(outcome.optionId or "")
    end
    return ""
end

function capstan_acp_active()
    return active ~= nil
end

function capstan_acp_disconnect()
    if active and not active.finished then
        acp.cancel()
        active.finished = true
        active = nil
    end
    for session_id in pairs(sessions) do
        mcp_client.close_scope(session_id)
    end
    sessions = {}
end
