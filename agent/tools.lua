local json = require("vendor.rxi.json")
local hooks = require("agent.hooks")
local logging = require("agent.logging")

local M = {}

function M.collect()
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

function M.names(tools)
    local names = {}
    for _, tool in ipairs(tools or {}) do
        if tool["function"] and tool["function"].name then
            table.insert(names, tool["function"].name)
        end
    end
    table.sort(names)
    return table.concat(names, ",")
end

local function find_plugin_tool(tool_name)
    if not _G.plugins then return nil end
    for _, p in pairs(_G.plugins) do
        if p.tool and p.tool.name == tool_name then
            return p
        end
    end
    return nil
end

local function call_plugin_tool(tool_name, args)
    local p = find_plugin_tool(tool_name)
    if not p then
        if not _G.plugins then return "No plugins loaded", false end
        return "Unknown tool: " .. tool_name, false
    end
    if type(p.handler) ~= "function" then
        return "Tool " .. tool_name .. " failed: plugin has no handler", false
    end

    local ctx = {
        input = "/" .. tool_name,
        command = p.command,
        args = {},
        tool_args = args,
    }
    function ctx:replace(ui_val, llm_val)
        return ui_val, llm_val or ui_val
    end
    local ok, ui_result, llm_result = pcall(p.handler, ctx)
    if not ok then
        return "Tool " .. tool_name .. " failed: " .. tostring(ui_result), false
    end
    return llm_result or ui_result, true
end

local function tool_permission_name(tool_name)
    local p = find_plugin_tool(tool_name)
    if p and p.tool and p.tool.permission and p.tool.permission ~= "" then
        return p.tool.permission
    end
    return tool_name
end

local function decode_tool_arguments(raw)
    local ok, decoded = pcall(json.decode, raw or "{}")
    if not ok then
        return nil, "Invalid JSON arguments: " .. tostring(decoded)
    end
    if type(decoded) ~= "table" then
        return nil, "Invalid tool arguments: expected object"
    end
    return decoded, nil
end

local function is_absolute_path(path)
    return type(path) == "string" and path:sub(1, 1) == "/"
end

local function configured_workdir()
    if _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir ~= "" then
        return _G.capstan.workdir
    end
    return nil
end

local function expand_home_path(path)
    if type(path) ~= "string" then return path end
    if path ~= "~" and path:sub(1, 2) ~= "~/" then return path end
    local home = os.getenv("HOME")
    if not home or home == "" then return path end
    return home .. path:sub(2)
end

local function normalize_path(path)
    if type(path) ~= "string" or path == "" then
        return path
    end

    path = expand_home_path(path)
    if not is_absolute_path(path) then
        local workdir = configured_workdir()
        if not workdir or workdir == "" then
            return path
        end
        path = workdir:gsub("/+$", "") .. "/" .. path
    end

    local parts = {}
    for part in path:gmatch("[^/]+") do
        if part == "." then
        elseif part == ".." then
            if #parts > 0 then
                table.remove(parts)
            end
        else
            table.insert(parts, part)
        end
    end

    return "/" .. table.concat(parts, "/")
end

local function tool_call_target(tool_name, args)
    if tool_name == "shell" and _G.capstan and type(_G.capstan.workdir) == "string" and _G.capstan.workdir ~= "" then
        return _G.capstan.workdir
    end
    return args.command or args.path or args.url or args.uri or tool_name
end

local function normalize_permission_target(permission_tool, target)
    if permission_tool == "file_read" or permission_tool == "file_write" then
        return normalize_path(target)
    end
    return target
end

local function collapse_home_path(path)
    if type(path) ~= "string" or path == "" then return path end
    local home = os.getenv("HOME")
    if not home or home == "" then return path end
    home = home:gsub("/+$", "")
    if path == home then return "~" end
    if path:sub(1, #home + 1) == home .. "/" then
        return "~" .. path:sub(#home + 1)
    end
    return path
end

local function tool_display_target(tool_name, args, target)
    if tool_name == "shell" then
        return collapse_home_path(target)
    end
    return target
end

local function tool_display_command(tool_name, args)
    if tool_name ~= "shell" then return nil end
    local command = args and args.command
    if type(command) ~= "string" or command == "" then return nil end
    return command
end

local function tool_status_suffix(status, display_command)
    if display_command then
        return status .. "\n  $ " .. display_command .. "\n\n"
    end
    return status .. "\n\n"
end

function M.process(current_msgs, combined_tools, tool_calls, assistant_text, continue_fn)
    logging.runtime_log("tools", string.format("received %d tool call(s)", #tool_calls))
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
        local result_content
        local args, decode_err = decode_tool_arguments(tc.arguments)

        if decode_err then
            result_content = decode_err
            logging.runtime_log("tool", string.format("invalid_args name=%s error=%s", tc.name, logging.compact(decode_err, 240)))
            agent.append(string.format("\n⚙ %s: invalid arguments — error\n\n", tc.name), "agent")
        else
            local target = tool_call_target(tc.name, args)
            local permission_tool = tool_permission_name(tc.name)
            local call_ctx = hooks.run("before_tool_call", {
                name = tc.name,
                args = args,
                target = target,
                permission_tool = permission_tool,
                raw_arguments = tc.arguments,
                tool_call = tc,
            })
            local tool_name = call_ctx.name or tc.name
            args = call_ctx.args or args
            target = call_ctx.target or target
            permission_tool = call_ctx.permission_tool or permission_tool
            target = normalize_permission_target(permission_tool, target)
            local display_target = tool_display_target(tool_name, args, target)
            local display_command = tool_display_command(tool_name, args)
            if display_command then
                logging.runtime_log("tool", string.format("call name=%s target=%s display=%s command=%s args=%s", tool_name, target, display_target, logging.compact(display_command, 240), tc.arguments or ""))
            else
                logging.runtime_log("tool", string.format("call name=%s target=%s args=%s", tool_name, target, tc.arguments or ""))
            end
            local perm = permit.check(permission_tool, target)
            logging.runtime_log("permit", string.format("tool=%s call=%s target=%s decision=%s", permission_tool, tool_name, target, perm))

            agent.append(string.format("\n⚙ %s: %s ", tool_name, display_target), "agent")

            if perm == "deny" then
                result_content = "Permission denied for " .. tool_name .. " " .. target
                agent.append(tool_status_suffix("— denied", display_command), "agent")
            elseif perm == "ask" then
                local decision = permit.prompt(permission_tool, target)
                logging.runtime_log("permit", string.format("tool=%s call=%s target=%s prompt=%s", permission_tool, tool_name, target, decision))
                if decision == "deny" then
                    result_content = "User denied " .. tool_name .. " " .. target
                    agent.append(tool_status_suffix("— denied by user", display_command), "agent")
                else
                    if decision == "always" then
                        permit.grant(permission_tool, target, true)
                        permit.save()
                    end
                    local tool_ok
                    result_content, tool_ok = call_plugin_tool(tool_name, args)
                    if tool_ok then
                        logging.runtime_log("tool", string.format("done name=%s target=%s bytes=%d", tool_name, target, #(result_content or "")))
                        agent.append(tool_status_suffix("— done", display_command), "agent")
                    else
                        logging.runtime_log("tool", string.format("error name=%s target=%s error=%s", tool_name, target, logging.compact(result_content or "", 240)))
                        agent.append(tool_status_suffix("— error", display_command), "agent")
                    end
                end
            else
                local tool_ok
                result_content, tool_ok = call_plugin_tool(tool_name, args)
                if tool_ok then
                    logging.runtime_log("tool", string.format("done name=%s target=%s bytes=%d", tool_name, target, #(result_content or "")))
                    agent.append(tool_status_suffix("— done", display_command), "agent")
                else
                    logging.runtime_log("tool", string.format("error name=%s target=%s error=%s", tool_name, target, logging.compact(result_content or "", 240)))
                    agent.append(tool_status_suffix("— error", display_command), "agent")
                end
            end
            local result_ctx = hooks.run("after_tool_call", {
                name = tool_name,
                args = args,
                target = target,
                permission_tool = permission_tool,
                result = result_content,
                tool_call = tc,
            })
            result_content = result_ctx.result
        end

        if result_content then
            table.insert(current_msgs, {
                role = "tool",
                tool_call_id = tc.id,
                content = result_content
            })
        end
    end

    logging.runtime_log("tools", "continuing with tool results")
    continue_fn(current_msgs, combined_tools)
end

return M
