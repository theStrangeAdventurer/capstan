---@meta

---@alias CapstanOAuthMethodType "oauth_device"
---@alias CapstanRole "system"|"user"|"assistant"|"tool"|"developer"

---@class CapstanPlugin
---@field id string
---@field name? string
---@field description? string
---@field command? string
---@field async? boolean
---@field history? boolean
---@field handler? fun(ctx: CapstanPluginContext): string?, string?
---@field autocomplete? CapstanAutocompleteSpec
---@field tool? CapstanToolSpec
---@field hooks? table<string, fun(ctx: table): table>
---@field auth? CapstanAuthAdapter
---@field source_path? string
---@field _source_path? string

---@class CapstanPluginContext
---@field input string
---@field command string
---@field args string[]
---@field replace fun(self: CapstanPluginContext, ui_val: string, llm_val?: string): string, string

---@class CapstanAutocompleteSpec
---@field title? string
---@field limit? integer
---@field multi? boolean
---@field fetch? fun(ctx: CapstanPluginContext): table[]

---@class CapstanToolSpec
---@field name string
---@field description string
---@field parameters table
---@field permission? string

---@class CapstanAuthAdapter
---@field provider string
---@field methods? CapstanAuthMethod[]
---@field authorize fun(method?: string, ctx?: CapstanPluginContext): CapstanCredential?, string?
---@field refresh? fun(credential: CapstanCredential): CapstanCredential?, string?

---@class CapstanAuthMethod
---@field type CapstanOAuthMethodType
---@field label? string
---@field fields? table[]

---@class CapstanCredential
---@field type "oauth"
---@field access string
---@field refresh? string
---@field expires? number
---@field metadata? table

---@class CapstanAuthApi
---@field get fun(provider_id: string): CapstanCredential?
---@field set fun(provider_id: string, credential: CapstanCredential): boolean, string?
---@field remove fun(provider_id: string): boolean, string?
---@field list fun(): table<string, CapstanCredential>
---@field redacted fun(provider_id: string): table?

---@class CapstanRuntime
---@field workdir string
---@field config table
---@field runtime_options table
---@field state table
---@field auth CapstanAuthApi
---@field skills_summary string
---@field skill_roots string[]
---@field state_path fun(relative?: string): string?
---@field state_dir fun(): string?
---@field state_ensure_dir fun(): boolean
---@field config_path fun(relative?: string): string?
---@field config_dir fun(): string?
---@field secure_write_file fun(path: string, content: string): boolean, string?
---@field now_ms fun(): number
---@field realpath fun(path: string): string?, string?
---@field path_join fun(base: string, relative?: string): string?
---@field log fun(category: string, message: string, level?: string)
---@field models CapstanModelsApi
---@field agent CapstanAgentApi
---@field mcp CapstanMcpApi

---@class CapstanModelsApi
---@field list fun(provider_name?: string): table[]?, string?
---@field list_all fun(): table[]
---@field set fun(provider_name: string, model: string): boolean?, string?
---@field set_weak fun(provider_name: string, model: string): boolean?, string?
---@field set_profile fun(profile_name: string, provider_name: string, model: string): boolean?, string?
---@field effective fun(profile_name?: string): table?

---@class CapstanAgentApi
---@field run fun(opts: table, callbacks?: table): boolean, string?
---@field set_profile fun(name: string): string?, string?
---@field get_profile fun(): string
---@field clear_profile fun()
---@field refresh_status fun()
---@field profiles fun(): string[]
---@field reasoning_effort fun(profile_name?: string): string?

---@class CapstanMcpApi
---@field tick fun(max_steps?: integer): integer

---@class HttpResponse
---@field status integer
---@field body string
---@field headers table<string, string>

---@class HttpApi
---@field get fun(url: string, headers?: table<string, string>, timeout_ms?: integer): string?, string?
---@field post fun(url: string, body: string, headers?: table<string, string>, timeout_ms?: integer): string?, string?
---@field post_response fun(url: string, body: string, headers?: table<string, string>, timeout_ms?: integer): HttpResponse?
---@field post_stream fun(url: string, body: string, headers: table<string, string>, callback: fun(raw: string?, done: boolean, err?: string, body?: string)): integer?

---@class AgentGlobal
---@field append fun(text: string, role?: string)
---@field set_info fun(provider: string, model: string)
---@field set_profile_info fun(profile: string)
---@field set_usage fun(prompt_tokens: integer, completion_tokens: integer, total_tokens: integer, context_limit?: integer)
---@field set_thinking fun(active: boolean)

---@class PermitGlobal
---@field check fun(tool: string, target: string): string
---@field prompt fun(tool: string, target: string): string
---@field grant fun(tool: string, pattern: string, persistent?: boolean)
---@field save fun()

---@class PopupGlobal
---@field info fun(title: string, body: string)
---@field error fun(title: string, body: string)

---@class McpGlobal
---@field spawn fun(command: string, args?: string[], opts?: table): integer?, string?
---@field send fun(handle: integer, line: string): boolean?, string?
---@field recv fun(handle: integer, timeout_ms?: integer): string?, string?
---@field alive fun(handle: integer): boolean
---@field kill fun(handle: integer): boolean?, string?

---@type CapstanRuntime
capstan = capstan

---@type table<string, CapstanPlugin>
plugins = plugins

---@type HttpApi
http = http

---@type AgentGlobal
agent = agent

---@type PermitGlobal
permit = permit

---@type PopupGlobal
popup = popup

---@type McpGlobal
mcp = mcp
