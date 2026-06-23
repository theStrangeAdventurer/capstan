local M = {}

local function state()
    if not _G.capstan then _G.capstan = {} end
    if type(_G.capstan.state) ~= "table" then
        _G.capstan.state = {}
    end
    return _G.capstan.state
end

local function lua_quote(value)
    local s = tostring(value or "")
    s = s:gsub("\\", "\\\\")
         :gsub("\n", "\\n")
         :gsub("\r", "\\r")
         :gsub("\t", "\\t")
         :gsub("\"", "\\\"")
    return "\"" .. s .. "\""
end

function M.model_for(provider_name)
    local st = state()
    if type(st.models) ~= "table" then return nil end
    local value = st.models[provider_name]
    if type(value) == "string" and value ~= "" then
        return value
    end
    return nil
end

function M.set_model(provider_name, model)
    local st = state()
    if type(st.models) ~= "table" then
        st.models = {}
    end
    st.models[provider_name] = model
    return M.save()
end

function M.save()
    if not (_G.capstan and _G.capstan.state_path and _G.capstan.state_ensure_dir) then
        return false, "state path is not available"
    end
    if not _G.capstan.state_ensure_dir() then
        return false, "cannot create state directory"
    end

    local path = _G.capstan.state_path("state.lua")
    if not path then
        return false, "cannot resolve state path"
    end

    local st = state()
    local file, err = io.open(path, "w")
    if not file then
        return false, err
    end

    file:write("return {\n")
    file:write("  models = {\n")
    if type(st.models) == "table" then
        local keys = {}
        for provider_name, model in pairs(st.models) do
            if type(provider_name) == "string" and type(model) == "string" then
                table.insert(keys, provider_name)
            end
        end
        table.sort(keys)
        for _, provider_name in ipairs(keys) do
            file:write("    [", lua_quote(provider_name), "] = ", lua_quote(st.models[provider_name]), ",\n")
        end
    end
    file:write("  },\n")
    file:write("}\n")
    file:close()
    return true, nil
end

return M
