local json = require("vendor.rxi.json")

local M = {}

M.provider = "deepseek"

M.providers = {
    deepseek = {
        api_key = os.getenv("DEEPSEEK_API_KEY"),
        endpoint = "https://api.deepseek.com/v1/chat/completions",
        model = "deepseek-chat",
    },
    openai = {
        api_key = os.getenv("OPENAI_API_KEY"),
        endpoint = "https://api.openai.com/v1/chat/completions",
        model = "gpt-4o",
    },
}

M.default_on_chunk = function(raw_event)
    local data = raw_event:match("^data: (.*)")
    if not data or data == "[DONE]" then return nil end
    local ok, event = pcall(json.decode, data)
    if not ok then return nil end
    if not event.choices or not event.choices[1] then return nil end
    local delta = event.choices[1].delta
    if delta.content then
        return {type = "text", content = delta.content}
    end
    if delta.tool_calls then
        return {type = "tool_calls", tool_calls = delta.tool_calls}
    end
    return nil
end

M.stream = function(provider_name, on_result)
    local buf = ""
    local accumulated_text = ""
    local tool_calls_accum = {}
    local prov = M.providers[provider_name]
    local on_chunk = prov.on_chunk or M.default_on_chunk

    local function process_chunk(chunk)
        if chunk.type == "text" and chunk.content then
            accumulated_text = accumulated_text .. chunk.content
            on_result({type = "text", content = chunk.content}, false)
        elseif chunk.type == "tool_calls" then
            for _, tc in ipairs(chunk.tool_calls) do
                local idx = tc.index or 0
                if not tool_calls_accum[idx] then
                    tool_calls_accum[idx] = {id = "", name = "", arguments = ""}
                end
                if tc.id then tool_calls_accum[idx].id = tc.id end
                if tc["function"] then
                    if tc["function"].name then
                        tool_calls_accum[idx].name = tool_calls_accum[idx].name .. tc["function"].name
                    end
                    if tc["function"].arguments then
                        tool_calls_accum[idx].arguments = tool_calls_accum[idx].arguments .. tc["function"].arguments
                    end
                end
            end
        end
    end

    return function(raw, is_done)
        if is_done then
            if #buf > 0 then
                local chunk = on_chunk(buf)
                if chunk then process_chunk(chunk) end
            end

            local final_calls = {}
            for _, tc in pairs(tool_calls_accum) do
                if tc.id ~= "" and tc.name ~= "" and tc.arguments ~= "" then
                    tc.name = tc.name:match("^%s*(.-)%s*$")
                    table.insert(final_calls, tc)
                end
            end

            on_result({
                text = accumulated_text,
                tool_calls = #final_calls > 0 and final_calls or nil
            }, true)
            return
        end

        buf = buf .. raw
        while true do
            local sep = buf:find("\n\n", 1, true)
            if not sep then break end
            local event = buf:sub(1, sep - 1)
            buf = buf:sub(sep + 2)
            local chunk = on_chunk(event)
            if chunk then process_chunk(chunk) end
        end
    end
end

local function collect_tools()
    local tools = {}
    if not _G.plugins then return tools end
    for _, p in pairs(_G.plugins) do
        if p.tool then
            table.insert(tools, {
                type = "function",
                ["function"] = {
                    name = p.tool.name,
                    description = p.tool.description,
                    parameters = p.tool.parameters,
                }
            })
        end
    end
    return tools
end

local function call_plugin_tool(tool_name, args)
    if not _G.plugins then return "No plugins loaded" end
    for _, p in pairs(_G.plugins) do
        if p.tool and p.tool.name == tool_name then
            local ctx_args = {}
            for _, v in pairs(args) do
                table.insert(ctx_args, tostring(v))
            end
            local ctx = {
                input = "/" .. tool_name,
                command = p.command,
                args = ctx_args,
                tool_args = args,
            }
            function ctx:replace(ui_val, llm_val)
                return ui_val, llm_val or ui_val
            end
            local ui_result, llm_result = p.handler(ctx)
            return llm_result or ui_result
        end
    end
    return "Unknown tool: " .. tool_name
end

local function process_tool_calls(current_msgs, combined_tools, tool_calls, assistant_text, continue_fn)
    local openai_tool_calls = {}
    for _, tc in ipairs(tool_calls) do
        table.insert(openai_tool_calls, {
            id = tc.id,
            type = "function",
            ["function"] = {name = tc.name, arguments = tc.arguments}
        })
    end

    table.insert(current_msgs, {
        role = "assistant",
        content = (assistant_text ~= "" and assistant_text or nil),
        tool_calls = openai_tool_calls
    })

    for _, tc in ipairs(tool_calls) do
        local args = {}
        local ok = pcall(json.decode, tc.arguments)
        if ok then args = json.decode(tc.arguments) end

        local target = args.command or args.path or tc.name
        local perm = permit.check(tc.name, target)

        agent.append(string.format("\n[tool %s: %s] ", tc.name, target), "agent")

        local result_content
        if perm == "deny" then
            result_content = "Permission denied for " .. tc.name .. " " .. target
            agent.append("— denied", "agent")
        elseif perm == "ask" then
            local decision = permit.prompt(tc.name, target)
            if decision == "deny" then
                result_content = "User denied " .. tc.name .. " " .. target
                agent.append("— denied by user", "agent")
            else
                if decision == "always" then
                    permit.grant(tc.name, target, true)
                    permit.save()
                end
                result_content = call_plugin_tool(tc.name, args)
                agent.append("— done", "agent")
            end
        else
            result_content = call_plugin_tool(tc.name, args)
            agent.append("— done", "agent")
        end

        if result_content then
            table.insert(current_msgs, {
                role = "tool",
                tool_call_id = tc.id,
                content = result_content
            })
        end
    end

    continue_fn(current_msgs, combined_tools)
end

_G.on_messages = function(messages)
    local active = M.providers[M.provider]
    agent.set_info(M.provider, active.model)

    local msgs = {}
    for _, m in ipairs(messages) do
        table.insert(msgs, {role = m.role, content = m.content})
    end

    local combined_tools = collect_tools()

    local function continue(current_msgs, tools)
        local function on_result(result, is_done)
            if not is_done then
                if result.type == "text" and result.content then
                    agent.append(result.content, "agent")
                end
                return
            end

            if result.tool_calls and #result.tool_calls > 0 then
                process_tool_calls(current_msgs, tools, result.tool_calls, result.text, continue)
            end
        end

        local body = json.encode({
            model = active.model,
            messages = current_msgs,
            tools = #tools > 0 and tools or nil,
            stream = true
        })

        http.post_stream(active.endpoint, body, {
            ["Content-Type"] = "application/json",
            ["Authorization"] = "Bearer " .. active.api_key
        }, M.stream(M.provider, on_result))
    end

    continue(msgs, combined_tools)
end

return M
