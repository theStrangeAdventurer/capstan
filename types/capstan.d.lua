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
---@field handler? fun(ctx: CapstanPluginContext): string?, string|CapstanImageResult?, boolean?
---@field autocomplete? CapstanAutocompleteSpec
---@field tool? CapstanToolSpec
---@field hooks? table<string, fun(ctx: table): table>
---@field auth? CapstanAuthAdapter
---@field source_path? string
---@field _source_path? string

---@class CapstanImageResult
---@field text string
---@field images {mime_type: string, data: string}[]

---@class CapstanPluginContext
---@field input string
---@field command string
---@field args string[]
---@field replace fun(self: CapstanPluginContext, ui_val: string, llm_val?: string|CapstanImageResult): string, string|CapstanImageResult
---@field error fun(self: CapstanPluginContext, ui_val: string, llm_val?: string): string, string, boolean

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

-- Internal scheduler API returned by require("agent.subagents"), not a model tool.
---@class CapstanSubagentHandle

---@class CapstanSubagentSubmitOptions
---@field is_cancelled? fun(): boolean
---@field notify? boolean Defaults to false for internal groups.
---@field telemetry_parent? table

---@class CapstanSubagentSnapshot
---@field done boolean True only after terminal publication.
---@field status "queued"|"running"|"completed"|"failed"|"cancelled"
---@field result? table Copied, bounded aggregate; never a validated review verdict.
---@field publication_error? string

---@class CapstanSubagentsApi
---@field run fun(args: table, run_ctx: table): string, boolean
---@field submit fun(args: table, run_ctx: table, options?: CapstanSubagentSubmitOptions): CapstanSubagentHandle?, string?
---@field result fun(handle: CapstanSubagentHandle): CapstanSubagentSnapshot?, string?
---@field cancel fun(handle: CapstanSubagentHandle): boolean, string?
---@field release fun(handle: CapstanSubagentHandle): boolean, string?
---@field poll fun()
---@field cancel_owner fun(owner: string)
---@field shutdown fun()

-- Internal review ledger API returned by require("agent.issues").
---@class CapstanIssueHandle

---@class CapstanReviewFinding
---@field id? string Existing issue ID only on re-review.
---@field severity "critical"|"high"|"medium"|"low"
---@field description string
---@field evidence string
---@field file? string
---@field start_line? integer
---@field end_line? integer

---@class CapstanReviewReport
---@field verdict "clean"|"findings"|"inconclusive"
---@field summary string
---@field findings CapstanReviewFinding[]
---@field checks {id:string,status:"open"|"resolved",evidence:string}[]

---@class CapstanIssuesApi
---@field use_store fun(store?: table) ACP serialized-session adapter.
---@field read fun(): table?, string?
---@field begin fun(run_id:string, baseline:string, snapshot:string, revision:integer): CapstanIssueHandle?, table|string
---@field snapshot fun(handle:CapstanIssueHandle, revision:integer, previous:string, next_snapshot:string): table?, string?
---@field record fun(handle:CapstanIssueHandle, revision:integer, snapshot:string, raw:string, transport:{ok:boolean,truncated:boolean}): table?, string?
---@field finish fun(handle:CapstanIssueHandle, revision:integer, snapshot:string, reason:string): table?, string?
---@field release fun(handle:CapstanIssueHandle)
---@field respond fun(args:table): table?, string?
---@field accept_risk fun(args:table): table?, string? Manual user action, not a model tool operation.
---@field display fun(id?:string): string

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
---@field issues_get fun(): string, integer Returns opaque session JSON and generation token.
---@field issues_set fun(json: string, token: integer): boolean Transactional, generation-checked session metadata write.
---@field append fun(text: string, role?: string)
---@field set_info fun(provider: string, model: string)
---@field set_review_status fun(label?: string) Separate upper purple review status; nil/empty hides it. Runtime owns labels.
---@field review_event fun(text: string) Append a purple, persisted background-review event at the end of history without changing foreground output sinks. Model receives labeled runtime data.
---@field set_running fun(active: boolean) Internal foreground input-slot control; does not clear review status.
---@field output_sink fun(new_segment?: boolean): (fun(text?: string): boolean)? Internal message-bound append; true creates a new assistant segment (e.g. repair resume). Calling the returned sink without text queries validity. History reset invalidates sink.
---@field set_profile_info fun(profile: string)
---@field set_usage fun(prompt_tokens: integer, completion_tokens: integer, total_tokens: integer, context_limit?: integer)
---@field set_thinking fun(active: boolean)

---Runtime-owned root lifecycle flag, not the independent background queue.
---Set while a root is deferred; clear before on_done. Native polls via
---agent_background_poll even without HTTP and preserves queued TUI input.
---@type boolean|nil
agent_root_pending = agent_root_pending

---Runtime cancellation entrypoint when agent_root_pending is true.
---Cancel only the current root and its owned reviewer; finalize exactly once.
---@type fun()|nil
agent_cancel_root = agent_cancel_root

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

-- Completion-review configuration, normalized once for each root run.
-- Run options > agent config > profile; false/nil disables, true uses defaults.
-- Nested runs do not review; enabled review requires capabilities.subagents.
---@class CapstanCompletionReviewConfig
---@field enabled? boolean Table default true; global feature default false.
---@field max_fix_cycles? integer Default 2; 0..30, zero reviews without repairs.
---@field max_duration_sec? number Default 900; positive finite, includes queue/repairs.
---@field max_requests? integer Default reviewer turns * (fix cycles + 1) + parent turns; shared requests including retries.
---@field reviewer? CapstanCompletionReviewerConfig

---@class CapstanCompletionReviewerConfig
---@field max_turns? integer Defaults to effective orchestrator max_turns; independent of public subagent turn caps.

---@alias CapstanCompletionReviewSetting boolean|CapstanCompletionReviewConfig
---@alias CapstanCompletionStatus 'ready'|'question'|'blocked'

---@class CapstanCompletionRequest
---@field status CapstanCompletionStatus Explicit ready requests review; question/blocked bypass it. Tool-free dialogue should use plain text.
---@field text string Nonblank, no NUL, at most 128 KiB; must be the sole tool call.
