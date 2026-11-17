# OpenCode Go

Select provider `opencode_go` with `OPENCODE_GO_API_KEY` or
`providers.opencode_go.api_key`. Environment keys override configuration as for
existing providers. No OAuth flow is added.

Default model: `deepseek-v4-pro`. Requests use the OpenAI-compatible translated
text endpoint `https://opencode.ai/inference/go/openai/v1/chat/completions`.
Model selection never switches to Zen, OpenRouter, or another billing endpoint.
Explicit provider endpoint overrides remain supported.

The shared runtime request path adds `User-Agent: capstan/<version>` (native
`capstan.version`, or `local` when unavailable) and `x-opencode-session` only
for Go. Session identity is the run's captured `process_owner`, inherited by
children and background work, not the currently visible UI session. Title runs
explicitly retain their owning conversation. Standard authorization, hooks,
streaming, errors and retry policy remain unchanged.

## Connect and select

Create a personal API key in the OpenCode Console with Go enabled. Run `/connect`
and choose **OpenCode Go**, or use `/connect opencode_go` directly. Paste the key
into the masked modal. Capstan saves it in an owner-only JSON file under its
state directory (directory 0700, file 0600). Failed writes are reported without
claiming a saved connection. No private plugin is required. See [Connect](connect.md).

`OPENCODE_GO_API_KEY` remains an alternative for automation. Do not paste the key
into chat or commit it in a project configuration.

Select `opencode_go` through `/models`, or run:

```sh
capstan run --provider opencode_go --model deepseek-v4-pro --prompt "Reply with exactly: ok"
```

The command above makes a real billable/provider request; it is not an offline
test. Go account limits, regional/privacy restrictions and Console Extra Usage
settings still apply. Capstan does not enable Extra Usage or change those settings.

## Model metadata

`/models` automatically loads the documented Go-specific public catalog at
`https://opencode.ai/inference/go/v1/models`; no extra provider configuration or
manually maintained model list is required after `/connect`. The general
`/inference/v1/models` catalog is not used, because it also contains non-Go models.

The Go endpoint was checked directly (HTTP 200) and against the official
[Go documentation](https://opencode.ai/docs/console/go.md#models). Its `data[]`
entries include `go`, `type`, `id`, `name`, `modalities`, `limit.context` and
`capabilities.reasoning.effort`. Only Go chat models with text output are shown.
The picker uses exact IDs and display names, context limits and recognized
reasoning levels; unknown values remain unknown. Native API family does not
exclude text models: Go documents translation through Chat Completions.
Catalog-supplied API URLs never alter the configured inference/billing endpoint.
Go Chat Completions uses `reasoning_effort` and preserves `reasoning_content`.

Catalog GETs are anonymous (no API key or conversation header), HTTPS-only,
without redirects, and identify Capstan via User-Agent. Successful catalogs are
cached in memory for five minutes. A failed refresh retains the last successful
list with a visible warning; a cold failure shows an error, not a fabricated
single-model list. Later attempts retry; failures do not permanently poison the
cache. `/models` still displays other providers if the Go catalog is unavailable.
This public list does not prove account access: subscription, privacy and region
restrictions can still reject an inference request.

Explicit `providers.opencode_go.models`, `reasoning_efforts`, `context_limit`,
`models_endpoint` and normal model/context environment overrides remain supported.
Manual metadata wins over discovery; keys are never sent to catalog overrides.
No list is persisted into user configuration. Explicit direct model selection
remains possible, but the picker does not invent catalog entries from defaults.

## Tests

`vendor/lua-5.5.0/src/lua test/test_opencode_go.lua` is offline and uses only fake
keys. It checks defaults, overrides, unknown context, request headers, captured
session continuity after a UI session switch, auxiliary/background/child runs,
and absence of Go headers on existing providers. Regression coverage also checks
background reviewer-shaped runs and multiple model families: the header must not
be gated by the selected model. Completion review passes its original process
owner through the same subagent/runtime path.

See [DeepSeek Harness discussion #5495](https://github.com/deepseek-ai/deepseek-harness/discussions/5495):
Go requires a stable per-conversation `x-opencode-session`, not a fixed global
value; auxiliary review calls are a documented failure path when session identity
is omitted. The built-in `opencode_go` adapter adds it; other providers do not
receive this provider-specific header automatically.

`test/test_opencode_catalog.lua` covers the public schema, metadata, model picker,
manual overrides, credential isolation, caching, visible failures and recovery.
Live inference/account acceptance is not tested.
