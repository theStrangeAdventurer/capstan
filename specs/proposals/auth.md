# Capstan OAuth Auth Module

Status: proposal only. Do not implement from this file unless explicitly asked.

## Summary

Add a generic OAuth-only auth module for Capstan. The module must support
provider plugins that implement browser OAuth and device-code OAuth flows. Core
owns credential storage, `/connect` orchestration, redacted status, refresh
lifecycle, and request hook invocation. Plugins own all provider-specific
details: client IDs, issuers, token endpoints, private subscription endpoints,
request rewrites, custom headers, and model availability.

This module intentionally does not manage API keys. Existing provider `api_key`
config/env behavior remains the only API-key path.

## Storage And Core API

- Store OAuth credentials in runtime state under a nested auth state directory:
  - `capstan.state_path("state/auth.lua")`
  - With `XDG_STATE_HOME`: `$XDG_STATE_HOME/capstan/state/auth.lua`
  - Fallback: `~/.local/state/capstan/state/auth.lua`
- Keep existing `state.lua` for non-secret preferences only.
- Ensure the nested `state/` directory exists before writing `auth.lua`.
- Write `auth.lua` with mode `0600` when the platform supports file mode
  changes.
- Add embedded Lua module `agent/auth.lua` and expose `capstan.auth`:
  - `capstan.auth.get(provider_id) -> credential|nil`
  - `capstan.auth.set(provider_id, credential) -> ok, err`
  - `capstan.auth.remove(provider_id) -> ok, err`
  - `capstan.auth.list() -> table`
  - `capstan.auth.redacted(provider_id) -> table|nil`
- OAuth credential shape:

```lua
{
  type = "oauth",
  access = "...",
  refresh = "...",
  expires = 1790000000000,
  metadata = {
    account_id = "...",
    issuer = "...",
    endpoint = "...",
  },
}
```

- `metadata` is provider-owned and must round-trip without core
  interpretation.
- Never log raw `access` or `refresh`. UI/logs may show only provider id, auth
  type, expiry, and redacted token fragments.

## Plugin Contract

- Let plugins register OAuth adapters through an optional `auth` table:

```lua
plugin.auth = {
  provider = "openai_codex",

  methods = {
    {
      type = "oauth_device",
      label = "ChatGPT Plus/Pro",
      fields = {},
    },
    {
      type = "oauth_browser",
      label = "ChatGPT Plus/Pro (browser)",
      fields = {},
    },
  },

  authorize = function(ctx, method, inputs) end,
  refresh = function(ctx, credential) end,
  apply_request = function(ctx, credential) end,
  models = function(ctx, credential) end,
}
```

- Supported method types:
  - `oauth_device`: terminal/headless flow where Capstan shows a URL/code and
    polls.
  - `oauth_browser`: browser flow where Capstan captures or accepts callback
    authorization.
- `fields` are user-input fields for `/connect`, not LLM prompts.
- Field shape:

```lua
{
  type = "text" | "select",
  key = "account",
  label = "Account",
  placeholder = "myorg-myaccount",
  required = true,
  options = {
    { label = "GitHub.com", value = "github.com" },
  },
  visible_when = { key = "deployment", equals = "enterprise" },
}
```

- `authorize(ctx, method, inputs)` returns either:
  - A completed OAuth credential, or
  - A pending result with `{ url, instructions, poll|callback }`.
- `refresh(ctx, credential)` returns an updated OAuth credential and persists
  through core.
- `apply_request(ctx, credential)` may mutate:
  - `ctx.endpoint`
  - `ctx.headers`
  - `ctx.request`
- Core must not contain provider-specific constants such as OpenAI client IDs,
  `ChatGPT-Account-Id`, Copilot headers, Snowflake account IDs, or private
  backend endpoints.

## Request Lifecycle And Commands

- In `agent/runtime.lua`, before `http.post_stream`:
  - Resolve credential with `capstan.auth.get(provider_name)`.
  - If credential exists and is expired, call plugin `refresh` once and persist
    the result.
  - Invoke plugin `apply_request` so the provider can modify endpoint, headers,
    and request body.
  - Continue using the final `endpoint`, `headers`, and JSON body with existing
    streaming.
- Coalesce concurrent refresh per provider in memory so rotating refresh tokens
  are not consumed twice.
- Add slash commands:
  - `/connect`: list providers that expose OAuth auth methods.
  - `/connect <provider>`: run default method or prompt when multiple methods
    exist.
  - `/connect <provider> <method>`: run a specific method.
  - `/logout <provider>`: remove stored OAuth credential.
  - `/auth`: show redacted auth status for connected providers.
- Missing OAuth credential should not block legacy API-key providers. If no
  OAuth credential exists, runtime falls back to current provider config/env
  behavior.
- If a provider requires OAuth and no credential exists, show a clear error
  suggesting `/connect <provider>`.

## Model Integration

- OAuth auth module does not own the `/models` UI.
- Add only a provider-scoped optional `models(ctx, credential)` hook.
- This hook returns model availability for the current OAuth credential.
- Existing `/models` behavior remains unchanged except it may ask this hook for
  the current provider.
- Future global `/models` fuzzy finder should consume this hook when collecting
  all models across providers.

## Initial OpenAI Codex Plugin

- Implement OpenAI subscription support as an experimental plugin, not core
  logic.
- Suggested plugin file: `plugins/openai_codex.lua`.
- The plugin owns:
  - OpenAI/Codex client id.
  - issuer/token URLs.
  - device/browser OAuth behavior.
  - access-token refresh.
  - account id extraction/storage in `metadata.account_id`.
  - request rewrite to the Codex subscription endpoint.
  - provider-specific headers such as account id.
  - subscription model filtering.
- Name/status should make experimental/private nature clear, e.g.
  `openai_codex` or `openai_codex_experimental`.

## Errors And Edge Cases

- Auth file parse failure:
  - Log a compact parse error.
  - Treat auth state as empty.
  - Do not overwrite the file until an explicit successful auth write.
- Refresh failure:
  - Keep old credential on disk.
  - Surface a visible provider auth error.
  - Do not log secrets.
- Expired credential without refresh function:
  - Fail with a clear reconnect message.
- Logout:
  - Remove only the selected provider credential.
  - Do not touch selected model state or provider config.
- Request transform failure:
  - Abort the request before `http.post_stream`.
  - Report plugin id/provider id and compact error.
- Browser OAuth callback may require a small core helper later. Device-code
  OAuth is acceptable for v1 if browser callback is too large.

## Test Plan

- `make test`:
  - Nested state path and directory creation.
  - Auth file write mode `0600` where available.
  - Credential validation and serialization.
  - Lua string escaping for auth state.
  - Redaction never exposes raw `access` or `refresh`.
  - Unknown `metadata` round-trips unchanged.
- `make test-http-lua`:
  - `capstan.auth.get/set/remove/list/redacted`.
  - `/connect` stores OAuth credentials.
  - `/logout` removes only the selected provider credential.
  - Expired OAuth credential triggers plugin refresh before request.
  - Refresh result is persisted.
  - Failed refresh leaves old credential unchanged.
  - `apply_request` can mutate endpoint, headers, and request body.
  - Optional `models` hook can override current provider model listing.
- Regression tests:
  - Existing env/config `api_key` providers still work without `auth.lua`.
  - Existing selected model persistence in root `state.lua` is unchanged.
  - Runtime logs never include raw OAuth secrets.
  - Failed auth file parse does not crash startup.
