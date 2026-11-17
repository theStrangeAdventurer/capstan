# Connect: unified provider onboarding

Status: implemented. Native storage, Lua/provider regressions, standalone build
and full-binary PTY save/restart/cancel/corrupt-file scenarios passed on macOS.
Checks: `make test-oauth-store test-http-lua test-build test-connect` and
`git diff --check`. Live provider acceptance and Linux execution were not tested.

API-key persistence uses the existing native locked file store on macOS and
Linux. No Keychain integration, secret-service dependency, or automatic migration
from old Keychain entries remains. Those entries are not read or deleted.

`/connect` opens a picker with only **OpenCode Go** (`opencode_go`) and
**ChatGPT / Codex** (`openai_siwc`, built-in SIWC preview). The private
`openai_codex` plugin, DeepSeek, OpenRouter and other registered providers are
not shown. This is a menu visibility policy, not removal of provider support:
`/connect <provider-id>` still dispatches directly to any supported API-key or
registered OAuth provider. OAuth providers retain their own authorization
protocol, not their own entrypoint.

## API key flow

1. Select a provider; enter/paste its key into a dedicated masked modal.
2. Save into `capstan.state_path('credentials')/state.json` (normally
   `~/.local/state/capstan/credentials/state.json`, or beneath `XDG_STATE_HOME`).
   The directory is owner-only `0700`; data and lock files are `0600`.
3. Cancellation leaves the prior connection unchanged. Unsafe permissions,
   corrupt data, unavailable storage or failed writes produce a visible error;
   no fallback exports secrets into the environment or writes shell configs.
4. Report the credential source, never the key. Saving a key is not proof that
   the remote service accepts it; no paid inference request is made on save.

The versioned JSON contains a provider map of canonical endpoint and API key.
It is not executable configuration and is not encrypted: other processes running
as the same OS user and privileged processes may read it. Exclude it from shared
backups and repositories. Existing environment/config-based setup remains
supported. An explicit successful connect prefers the saved file during the
current process; on future launches environment/configuration retain precedence,
and `/connect` warns when an environment key will override the saved key.

## Boundaries

- Reuse `capstan.oauth.store_open`: bounded lock acquisition, safe native
  directory/file checks, exclusive temporary file, fsync and atomic rename.
  Read/modify/write always holds the lock; malformed JSON is never overwritten.
- Reads are not cached across requests, so another process's update/logout is
  observed. No key in argv, model tools, chat-based entry or shell children.
- One credential resolver supplies inference and model catalog requests.
- Stored built-in keys are bound to their intended endpoint; changing an endpoint
  must not silently forward a saved key elsewhere.
- Explicitly selected credentials take effect immediately in the running process.
  Existing environment precedence on future launches must be disclosed.
- Masked entry does not touch normal draft/history, model context, logs, telemetry,
  or persisted configuration. Bracketed paste cannot submit on pasted Enter.
- `/logout <provider>` removes managed credentials; environment/config values
  cannot be deleted by pretending to log them out, and their presence is reported.
- SIWC OAuth account storage stays separate in its existing locked JSON file;
  the native storage mechanism is shared, not the credential schema.

## Tests

Use fake values and isolated fixture homes, never real credential files. Lua
regressions cover storage errors, lock timeout, corruption preservation, provider
isolation, reload, external updates/logout, environment precedence, canonical
endpoints, and shared catalog/request resolution. Native store tests cover file
permissions, process contention and symlink rejection. Full-binary PTY tests
exercise real file writes, 0700/0600, restart, cancellation and corrupt-file
failure, checking that keys are absent from terminal output, logs and history.
