local serialize = require("agent.lua_serialize")

local M = {}

local function state()
    if not _G.capstan then _G.capstan = {} end
    if type(_G.capstan.state) ~= "table" then
        _G.capstan.state = {}
    end
    return _G.capstan.state
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

function M.model_reasoning_effort(provider_name)
    local st = state()
    if type(st.model_reasoning_efforts) ~= "table" then return nil end
    local value = st.model_reasoning_efforts[provider_name]
    if type(value) == "string" and value ~= "" then return value end
    return nil
end

function M.provider()
    local st = state()
    if type(st.provider) == "string" and st.provider ~= "" then
        return st.provider
    end
    return nil
end

function M.weak_model()
    local st = state()
    local value = st.weak_model
    if type(value) == "table" and
       type(value.provider) == "string" and value.provider ~= "" and
       type(value.model) == "string" and value.model ~= "" then
        return {
            provider = value.provider,
            model = value.model,
            reasoning_effort = type(value.reasoning_effort) == "string" and
                value.reasoning_effort or nil,
        }
    end
    return nil
end

function M.profile_models()
    local st = state()
    local out = {}
    if type(st.profile_models) ~= "table" then return out end
    for profile_name, value in pairs(st.profile_models) do
        if type(profile_name) == "string" and
           type(value) == "table" and
           type(value.provider) == "string" and value.provider ~= "" and
           type(value.model) == "string" and value.model ~= "" then
            out[profile_name] = {
                provider = value.provider,
                model = value.model,
                reasoning_effort = type(value.reasoning_effort) == "string" and
                    value.reasoning_effort or nil,
            }
        end
    end
    return out
end

function M.vcs_for_workspace(workspace_root)
    local st = state()
    if type(st.vcs_by_workspace) ~= "table" then return nil end
    local value = st.vcs_by_workspace[workspace_root]
    return type(value) == "string" and value ~= "" and value or nil
end

function M.set_vcs_for_workspace(workspace_root, adapter)
    if type(workspace_root) ~= "string" or workspace_root == "" then
        return false, "workspace root is not available"
    end
    local st = state()
    if type(st.vcs_by_workspace) ~= "table" then st.vcs_by_workspace = {} end
    st.vcs_by_workspace[workspace_root] = adapter
    return M.save()
end

function M.set_provider(provider_name)
    local st = state()
    st.provider = provider_name
    return M.save()
end

function M.set_model(provider_name, model, reasoning_effort)
    local st = state()
    if type(st.models) ~= "table" then
        st.models = {}
    end
    st.models[provider_name] = model
    if type(st.model_reasoning_efforts) ~= "table" then
        st.model_reasoning_efforts = {}
    end
    st.model_reasoning_efforts[provider_name] = reasoning_effort
    st.provider = provider_name
    return M.save()
end

function M.set_weak_model(provider_name, model, reasoning_effort)
    local st = state()
    st.weak_model = {
        provider = provider_name,
        model = model,
        reasoning_effort = reasoning_effort,
    }
    return M.save()
end

function M.set_profile_model(profile_name, provider_name, model, reasoning_effort)
    local st = state()
    if type(st.profile_models) ~= "table" then
        st.profile_models = {}
    end
    st.profile_models[profile_name] = {
        provider = provider_name,
        model = model,
        reasoning_effort = reasoning_effort,
    }
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
    if type(st.provider) == "string" and st.provider ~= "" then
        file:write("  provider = ", serialize.quote(st.provider), ",\n")
    end
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
            file:write("    [", serialize.quote(provider_name), "] = ", serialize.quote(st.models[provider_name]), ",\n")
        end
    end
    file:write("  },\n")
    file:write("  model_reasoning_efforts = {\n")
    if type(st.model_reasoning_efforts) == "table" then
        local keys = {}
        for provider_name, effort in pairs(st.model_reasoning_efforts) do
            if type(provider_name) == "string" and type(effort) == "string" then
                table.insert(keys, provider_name)
            end
        end
        table.sort(keys)
        for _, provider_name in ipairs(keys) do
            file:write("    [", serialize.quote(provider_name), "] = ",
                serialize.quote(st.model_reasoning_efforts[provider_name]), ",\n")
        end
    end
    file:write("  },\n")
    if type(st.weak_model) == "table" and
       type(st.weak_model.provider) == "string" and
       type(st.weak_model.model) == "string" then
        file:write("  weak_model = {\n")
        file:write("    provider = ", serialize.quote(st.weak_model.provider), ",\n")
        file:write("    model = ", serialize.quote(st.weak_model.model), ",\n")
        if type(st.weak_model.reasoning_effort) == "string" then
            file:write("    reasoning_effort = ", serialize.quote(st.weak_model.reasoning_effort), ",\n")
        end
        file:write("  },\n")
    end
    file:write("  vcs_by_workspace = {\n")
    if type(st.vcs_by_workspace) == "table" then
        local keys = {}
        for workspace_root, adapter in pairs(st.vcs_by_workspace) do
            if type(workspace_root) == "string" and type(adapter) == "string" then
                table.insert(keys, workspace_root)
            end
        end
        table.sort(keys)
        for _, workspace_root in ipairs(keys) do
            file:write("    [", serialize.quote(workspace_root), "] = ",
                serialize.quote(st.vcs_by_workspace[workspace_root]), ",\n")
        end
    end
    file:write("  },\n")
    file:write("  profile_models = {\n")
    if type(st.profile_models) == "table" then
        local keys = {}
        for profile_name, value in pairs(st.profile_models) do
            if type(profile_name) == "string" and
               type(value) == "table" and
               type(value.provider) == "string" and
               type(value.model) == "string" then
                table.insert(keys, profile_name)
            end
        end
        table.sort(keys)
        for _, profile_name in ipairs(keys) do
            local value = st.profile_models[profile_name]
            file:write("    [", serialize.quote(profile_name), "] = {\n")
            file:write("      provider = ", serialize.quote(value.provider), ",\n")
            file:write("      model = ", serialize.quote(value.model), ",\n")
            if type(value.reasoning_effort) == "string" then
                file:write("      reasoning_effort = ", serialize.quote(value.reasoning_effort), ",\n")
            end
            file:write("    },\n")
        end
    end
    file:write("  },\n")
    file:write("}\n")
    file:close()
    return true, nil
end

return M
