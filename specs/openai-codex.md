# Built-in OpenAI Codex via Sign in with ChatGPT

**Status: IMPLEMENTED PREVIEW — offline validation passed on macOS arm64.**

Validated: `make test`, `make test-oauth-crypto`, `make test-oauth-native`,
`make test-oauth-store`, `make test-http-lua`, `make test-build`, and
`git diff --check`. Final Lua/build checks include account-bound history and
configured reasoning support. This does not establish live service acceptance.

Implemented: official SIWC registration/refresh/revocation, strict RS256 OIDC
verification, locked JSON account storage, authenticated visible-model discovery,
Responses text/function tools with native history persistence, and embedded
provider/commands. HTTP credential paths are HTTPS-only with redirects disabled.
macOS uses system Security.framework for RS256/randomness and CommonCrypto for
SHA-256; OpenSSL is neither built nor linked there. Linux retains checksum-pinned
OpenSSL 3.5.9 libcrypto with static linkage. Native helpers and Lua policies have
offline tests; both backends preserve the same strict verification interface.

## Preview usage and current limits

In the interactive interface run `/connect openai_siwc`, then open the displayed
**Continue with ChatGPT** URL in your browser. The listener is bound to local
127.0.0.1; remote terminal/SSH sign-in is not supported by this initial UI.
Use `/auth` to find saved account IDs; `/connect openai_siwc <account-id>`
reauthorizes that registration. `/models` selects an account-visible model.
`/logout openai_siwc [account-id]` revokes when possible and clears local tokens;
it explicitly reports unconfirmed remote revocation. CLI runs can reuse a saved
connection with `--provider openai_siwc`; CLI sign-in is not implemented.

Current limits: text and local function tools only; attachments are rejected,
not silently ignored. Explicit reasoning effort is supported when declared in
`providers.openai_siwc.reasoning_efforts[model_id]` (or the provider's existing
`default_reasoning_efforts` configuration). Without configured capabilities the
server default is used; unsupported selections fail explicitly. The public SIWC
catalog fields used here establish visibility/identity, not capability metadata;
context limits remain unknown unless explicitly configured through the existing
provider model/context-limit settings. Browser launch is manual, sign-in expires after five
minutes, and the command UI does not yet wire immediate Esc cancellation.
`invalid_grant` requires a new manual sign-in; omitted refresh scopes fail closed.
A logout prevents subsequent requests but does not recall an already transmitted
request. Persisted native history is bound to its account ID; a mismatched or
pre-release unbound history requires a new conversation. This fail-closed policy
has offline regression coverage. Live OpenAI acceptance and Linux/x86_64 crypto
linkage remain unverified.
The private `openai_codex` plugin and its credentials are not migrated or removed.

The sections below describe the target security/behavior contract. Requirements
beyond this implemented preview remain work items, not claims of completion.

This specification covers the official open-source token-sharing Sign in with
ChatGPT (SIWC) flow and public Responses API. It does not document, import, or
endorse the private `openai_codex` plugin's authentication or backend protocol.
No ChatGPT `backend-api` endpoint, private OAuth client constant, client secret,
partner API key, or Codex app-server dependency belongs in this implementation.

## Scope and provider identity

Recommend provider ID **`openai_siwc`**, displayed as **OpenAI Codex (ChatGPT)**.
Keep `openai_codex` available to the existing private plugin; do not alias either
ID or silently migrate credentials. New provider selection is explicit through
existing configuration, `/models`, profile model selection, and CLI overrides.
The sign-in action must say **Continue with ChatGPT**.

Migration is opt-in: connect `openai_siwc` with a new official registration,
validate it, select an account-specific model, then update the chosen default
and any profile bindings. Old sessions retain their provider identity. Do not
copy private tokens/client IDs or rewrite old history into an assumed compatible
format. Users may independently remove their private integration afterwards.

A distinct ID avoids correctly scoped private hooks running twice. It cannot
sandbox trusted plugins with unscoped hooks. The built-in adapter must own final
credential injection, endpoint validation, and serialization after generic
request hooks; reject incompatible modifications rather than dispatching a
credential-bearing request to an altered origin. Document that unscoped private
hooks must be disabled or corrected. Reject duplicate ownership of the new ID.

## Existing implementation seams (inspected, not new capabilities)

- `agent/provider_config.lua` builds provider configuration and common options;
  current built-ins use chat completions.
- `agent/runtime.lua` builds requests, runs `before_request`, and dispatches
  `http.post_stream`. Root agents, child agents, compaction and title generation
  must all use the same provider adapter, not command-specific rewrites.
- `agent/stream.lua` already has a provider parser override and shared SSE
  framing. Its current chunk vocabulary needs an explicit terminal outcome for
  Responses; transport EOF alone must not imply success for this provider.
- `agent/models.lua` expects `data[]`, sorts IDs, authenticates with `api_key`,
  and caches by URL. None is a sufficient SIWC catalog policy.
- `agent/auth.lua` stores one credential per provider in `state/auth.lua` through
  `capstan.secure_write_file`. It loads executable Lua and does whole-file
  read/modify/write without interprocess serialization. Its redacted view
  forwards `metadata` unchanged and exposes token prefixes/suffixes; do not put
  new account tokens into that metadata or reuse that display unchanged.
- `/connect` discovers plugin auth descriptors, defaults to method `device`,
  then saves one returned credential. `/logout` only removes local credentials.
  Extend these contracts compatibly; SIWC uses browser/loopback, not device code.
- `src/plugins_runtime.c` exports the secure file writer; native code already
  uses OS randomness and POSIX file locking in other modules. These are patterns,
  not an existing OAuth/JOSE implementation.
- `Makefile` directly links vendored Lua/ncurses archives, `-lm`, and system
  `-lcurl`. Inspected native sources/headers expose no SHA-256/JWKS verifier or
  OAuth loopback API. libcurl TLS does not provide a portable JOSE API.
- `test/test_auth_lua.c` tests the legacy set/get/remove/redaction contract.
  Preserve this contract for other providers.

## Policy ownership

Use one built-in provider module (proposed `agent/openai_siwc.lua`) for official
endpoints, scopes, account lifecycle, catalog mapping and Responses adaptation.
Expose it to existing auth commands through a thin built-in plugin descriptor
(proposed `plugins/openai_siwc.lua`). Do not implement security decisions in
best-effort hooks: `agent/hooks.lua` catches hook exceptions and continues.
Authentication, claim verification, refresh persistence, endpoint restrictions,
and terminal-response validation must fail closed through explicit errors.

Native helpers own randomness, cryptographic verification, bounded loopback
I/O and lock/file primitives. Lua owns OAuth state transitions and provider
semantics. Credentials stay out of model context, session history, normal
configuration, logs, telemetry, support output and tool results. Account UI uses
an explicit safe projection, never a credential table.

## Native crypto: platform implementations

The `oauth_crypto.h` API owns bounded base64url, PKCE, randomness and RS256
verification. OIDC claims/JWKS policy stays in Lua and does not vary by platform.

On **macOS**, use system APIs already available alongside Keychain:
`SecRandomCopyBytes`, `CC_SHA256`, `SecKeyCreateWithData`, and
`SecKeyVerifySignature` with `kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256`.
Public JWK integers are serialized to bounded PKCS#1 DER solely for key import;
RSA and hashing are not implemented in application code. No persistent Keychain
entry is created for the public verification key. Neither `make` nor `build.sh`
downloads, builds or links OpenSSL on macOS.

On **Linux**, keep the pinned OpenSSL 3.5.9 static libcrypto implementation,
without libssl, tools or loadable providers. The archive remains hidden from
libcurl's dynamic TLS backend using linker symbol exclusion. Linux size reduction
and build verification are separate work; this change does not claim them done.

Base64url encoding/strict decoding is small shared serialization code, tested
against fixed vectors, canonical-bit rejection and boundary sizes. Existing RFC
7515 RS256 and RFC 7636 PKCE fixtures test the same contract on either backend.
Do not replace signature verification with JWT decoding, shell crypto commands,
or hand-written RSA/bignums/SHA implementations.

Provide a narrow native verifier: strict bounded compact-JWS parsing, strict
base64url, algorithm/key compatibility checks, signature verification over the
original encoded signing input, and a validated result/error. Reject `none`,
HMAC/asymmetric confusion, unsupported `crit`/detached payloads, malformed or
ambiguous JSON, duplicate security fields, duplicate matching keys and unknown
algorithms. Select keys from trusted JWKS by `kid`, compatible `kty`, `use` and
`key_ops`; never fetch token-supplied `jku`/`x5u`. No generic unsafe Lua crypto API
is required. Exact algorithm allowlist is a release-blocking decision below.

## Registration and sign-in state machine

1. Under a host-state lock, create or load an opaque, cryptographically random
   UUIDv4 `ext_agent_host_id` (recommended wire representation `urn:uuid:...`).
   Set RFC version/variant bits. Persist atomically before opening authorization.
   It identifies this installation/runtime host, not an account or machine
   fingerprint: no MAC address, hostname, email or hardware-derived value.
   Reuse across accounts, restarts and logout; do not clone it into release
   packages or fixtures. Copied runtime state needs an explicit new-host reset.
2. Bind a bounded native HTTP listener to **127.0.0.1**, port zero or another
   available port, before launching the system browser. Fix path
   `/auth/callback`; only the port may change across attempts. Never use
   `localhost`, wildcard interfaces, remote callbacks or a shell HTTP server.
3. Generate independent fresh CSPRNG `state`, `nonce`, and PKCE verifier for
   every attempt (32 random bytes each, base64url without padding yields a
   43-character verifier). Derive S256 challenge. Hold these and the exact
   redirect URI in memory with timeout/cancellation; do not persist the verifier.
4. Open `https://auth.openai.com/api/accounts/authorize` with URL-encoded values:
   `response_type=code`, `scope=openid profile email offline_access resource.invoke
   chatgpt.tokens.use.direct`, `resource=https://api.openai.com/v1`, `state`,
   `nonce`, `code_challenge_method=S256`, `code_challenge`, `redirect_uri`, and
   `ext_agent_host_id`. For initial registration use
   `client_id=dynamic_agent_client` and `agent_name_hint=Capstan`.
   For reauthorization use the saved issued ID, omit `agent_name_hint`, and
   optionally send that same account's retained `id_token_hint` and `login_hint`.
   An expired hint is allowed; it is never proof of authentication.
5. Accept only the expected HTTP method/path and valid query encoding, bounded
   sizes, and one value per security parameter. Validate state on both success
   and OAuth errors, including `access_denied`, before using callback values.
   Ignore unrelated requests without accepting them as success. Consume a valid
   attempt once, close listener, and reject replay. Validate Host against the
   bound loopback authority; callback HTML contains no tokens, external resources
   or reflected query text. Apply no-store/referrer protection.
6. A new callback must supply a nonempty issued client ID distinct from
   `dynamic_agent_client`. Returning callbacks may omit it; if supplied it must
   equal the pending account's ID. POST form data to
   `https://auth.openai.com/api/accounts/oauth/token`: `grant_type=authorization_code`,
   issued `client_id`, `code`, `code_verifier`, exact `redirect_uri`, and the same
   `resource`. No client secret. On `invalid_grant`, discard the code and start a
   fresh authorization, retaining the issued ID only in the pending flow until
   identity is validated; never retry the consumed code.
7. Fetch discovery from
   `https://auth.openai.com/.well-known/openid-configuration`; require issuer
   `https://auth.openai.com`, HTTPS endpoints and trusted origin policy. Obtain
   the published `jwks_uri` there; never guess it from private integrations.
   Verify ID-token signature, exact issuer, audience containing the issued ID,
   OIDC authorized-party rules for multiple audiences, numeric expiry, applicable
   not-before/issued-at bounds, and exact pending nonce. Use a small documented
   bounded clock skew (proposed 60 seconds), never indefinite grace.
8. Require a valid nonempty verified `sub`. Returning sign-in must match the
   selected registration's issuer/subject/client ID before replacement. Parse
   granted scopes from the token response, not callback scope or requested scope.
   Missing `chatgpt.tokens.use.direct` means identity-only, no inference; show an
   actionable permission error. Validate token type/expiry and response shapes.
9. Atomically persist the complete validated record before switching active
   account. Errors leave the previous active account and credentials intact.
   Missing/invalid discovery, keys, signature, claims or persistence are failures,
   not reasons to accept an unverified identity.

JWKS caching is bounded by cache policy; an unknown `kid` permits one refresh,
not an unbounded fetch loop. Unknown keys after refresh fail closed. Discovery,
JWKS, token and revocation requests use verified TLS with bounded time/size and
no cross-origin credential forwarding or unchecked redirects. Pin the issuer,
not a rotating signing key. Exact allowed discovery endpoint origins must be
confirmed against official metadata before release.

## Account storage and concurrency

Extend the existing auth service with account-aware operations while preserving
legacy single-provider callers. Recommended SIWC state is a versioned,
non-executable JSON document beneath the existing resolved state root, separate
from legacy `auth.lua`, with a native lock companion. This separation avoids
legacy whole-file writers losing new accounts and avoids executing stored
credential data. Do not create an unrelated home/config discovery mechanism.

Each record contains an internal stable account ID/label, verified `issuer` and
`subject`, issued `client_id`, host ID association, safe display email, access/
refresh/retained ID tokens, token type, granted scopes, reception timestamp,
expiry and generation. Registration identity is `(issuer, subject, client_id)`,
not email or subject alone. Workspace binding is implicit in the issued client;
do not infer workspace identity from email/sub. State also selects an active
record; the persistent host identity is separate from removable tokens.

Use a private directory (0700), owner-only files (0600), safe no-symlink opens,
regular-file/ownership checks, exclusive temporary files, flush/fsync, atomic
rename and directory synchronization. Report corrupt/unreadable storage; do not
interpret it as an empty registry and overwrite it. Do not export credentials
through `/auth`, raw metadata or token-prefix displays. Protect lock files too.

Use one stable POSIX advisory lock file for the SIWC registry (not the renamed
data inode) with bounded cancellable acquisition. This minimal global lock may
serialize different accounts; correctness is preferable to per-account lock
complexity initially. Every writer, including host creation, registration,
refresh, logout and account selection, participates. Under lock reload current
state and generation, then decide whether refresh is still necessary. Serialize
the token HTTP exchange and atomic replacement for the same renewable session;
a second process reuses the newly stored token instead of rotating again.
Native nonblocking lock attempts must yield to the event loop; a crashed process
releases its kernel lock. Keep network operations time-bounded.

Refresh near expiry using `grant_type=refresh_token`, the record's issued
`client_id`, current `refresh_token`, and `resource=https://api.openai.com/v1`;
omit `scope`. Validate response and update access token, expiry, granted scopes
and any replacement refresh/ID token together. Preserve an existing refresh
token only if protocol-valid omission is confirmed; never replace it with nil
accidentally. Validate any new ID token against the existing identity under
refresh-specific OIDC rules, not an invented new authorization nonce. Exact
refresh ID-token/scope omission semantics require the token reference before
implementation. Rotation succeeded but persistence failed is a visible
reauthorization/storage error; do not claim renewable success using stale disk
credentials. `invalid_grant` requires reauthorization, not a retry storm.

Pin each run and model request to a validated account ID and generation; a UI
switch affects new runs, never mixes credentials/history within an in-flight
request. Child requests use the same policy; isolated runtimes must not gain
credential access merely because a provider exists.

## Logout and remote revoke

`/logout openai_siwc` targets the selected account, not every saved account.
Stop/cancel its new and active requests, serialize with refresh, and POST to the
trusted discovery `revocation_endpoint` with refresh token,
`token_type_hint=refresh_token`, and issued `client_id`. Empty HTTP 200 is success,
including an already-invalid token. Network/5xx failures get bounded backoff
while the token remains available, with a local-logout option. Other failures
remain visible; do not blindly retry auth failures.

Always distinguish confirmed remote revocation from local sign-out. Clear
access, refresh and retained ID tokens locally; retain account/client mapping
and host ID. If remote revocation is unconfirmed, explicitly say so and direct
the user to ChatGPT Settings to disconnect the app. Local deletion failure is a
logout failure, not success. Use generation/tombstone checks to prevent a queued
refresh from resurrecting tokens; other processes recheck before dispatch.
Already transmitted requests cannot be recalled and must not be promised so.

## Authenticated model catalog

GET `https://api.openai.com/v1/models` using the selected account's current OAuth
Bearer token through the same refresh owner as inference. Parse **`models[]`**,
not `data[]`; show only `visibility == "list"`, preserve server order, display
`display_name`, send exact `slug` as `model`. Reject malformed entries safely.

Cache by provider, account/registration and credential generation, not URL alone.
Invalidate on switch, reauthorization and logout. Keep capability/context-limit
resolution in `agent/models.lua`; normalize only documented metadata and leave
unknown limits unknown. Do not hardcode example model IDs or fabricate reasoning
levels. A saved/configured model must be validated against account availability;
a failed catalog must surface an error, not silently select another provider or
advertise a configured model as authenticated availability.

## Responses request and history contract

Send OAuth Bearer credentials only to
`POST https://api.openai.com/v1/responses`. Every request has `store: false`,
`stream: true`, and an `input` array with the complete context required for this
turn. Never send `previous_response_id`; local response IDs are diagnostics,
not server conversation persistence. Use `instructions` or developer messages;
explicit `{type: "message", role: "system"}` is rejected on this route.

Build a provider-specific allowlist rather than forwarding chat-completions
fields. Omit `background`, `conversation`, `max_output_tokens`, `max_tool_calls`,
`metadata`, `moderation`, `multi_agent`, `prompt`, `prompt_cache_retention`,
`safety_identifier`, `temperature`, `top_logprobs`, `top_p`, `truncation`, and
`user`; also do not leak `messages`, `max_tokens`, routing or `stream_options`
from the common request. Explain unsupported user options rather than implying
that they control this provider. Existing local turn/tool/time budgets remain
operative. Reasoning options are passed only when supported and validated.

Convert existing tool definitions to namespace-grouped function/custom tools
(or documented `additional_tools` input items); recommend one stable `capstan`
namespace initially with a reversible registry mapping to existing tool names.
Preserve call IDs, namespace/name identity, arguments and matching tool outputs.
Do not concatenate ambiguous names. Execute through existing permission and tool
routing; local MCP tools remain local function calls, not hosted connectors.

History must preserve provider-native output items, including supported opaque
reasoning continuity data, alongside normal UI history. Re-send previous output
items and function-call outputs in order with required context. Do not flatten
structured calls/opaque reasoning into assistant text or use response IDs as a
substitute. Apply the same rules to persisted sessions, resume, compaction,
children and retries; define an explicit adapter/version marker so private
plugin items cannot be replayed accidentally. Exact namespace wire schema and
reasoning-item replay fields need official Responses schema fixtures before
implementation; the four SIWC pages do not fully define these structures.

Only text/images/files accepted by the model may be included. No audio/video,
Files upload API, transcription, image-generation tool, file search, Code
Interpreter, native computer use, hosted MCP/connectors, Responses `tool_search`
or top-level `programmatic_tool_calling`. Do not silently downgrade unsupported
attachments/tools. Web search is optional and account/model-policy dependent,
not required for the first implementation.

## Streaming and failure semantics

Reuse SSE framing and transport in the shared stream path; add typed Responses
mapping and a per-request terminal state. Accumulate tool argument fragments by
stable item/call identity, not arrival order; reconcile final items without
appending arguments twice. Emit UI text from `response.output_text.delta`.
Map usage input/output/cache/reasoning counters to the existing validated usage
contract without inventing measured values.

**Only `response.completed` authorizes a successful turn and tool dispatch.**
Read through the terminal event. EOF, `[DONE]`, text output, HTTP 200 or a complete
looking tool item alone is insufficient. `response.failed`, `response.incomplete`,
explicit error events, malformed required events, cancellation, timeout and
interrupted streams produce distinct failure results. Do not run partial tool
calls or finalize completion review on failed output. Handle completion exactly
once; contradictory terminal events are protocol errors.

Assistant message `phase` (`commentary` or `final_answer`) is preserved by the
native output normalizer, persisted history and request replay. Absent phases
remain absent; invalid phases fail closed rather than being silently discarded.
Offline tests cover stream snapshots, JSON history restoration and replay.

The SIWC adapter preserves JSON array types while copying native output and
explicitly marks schema arrays (`annotations`, reasoning `summary`/`content`,
message `content`, request `input`). Empty arrays must serialize as `[]`, not
`{}`, on subsequent turns and after history restoration. Legacy empty tables
in these fields are normalized on replay; non-array annotations fail closed.
Offline regression tests assert the encoded second-turn request, empty and
nonempty annotations, copy isolation, and restored history.

Late failures `subscription_sharing_usage_limit_exceeded` and
`subscription_sharing_usage_unavailable` have distinct `subscription_limit` and
`usage_unavailable` categories, not protocol errors. The former pauses the run
and links to Usage without guessing a reset time or an exhausted whole plan;
the latter preserves credentials and advises a later retry with bounded backoff.
Offline runtime tests cover both codes and exclude automatic immediate retries.
Preserve the safe structured code and
explain recovery, including https://chatgpt.com/settings/usage. Never refresh or
retry indefinitely to bypass quota. Maintain existing retry constraints: no
silent replay after visible output or tool side effects. Partial visible text
is marked incomplete, not silently promoted to successful persisted history.
Transport diagnostics and telemetry must reflect the semantic terminal result.

SIWC admission failures preserve the HTTP status and bounded diagnostic fields
from JSON `error.code`, `error.param`, `error.message`, or top-level `detail`,
plus a request ID when supplied. Network failures show the native cURL reason
and numeric code when available, rather than labeling all HTTP failures as
network errors. Empty, malformed and non-JSON bodies are reported without
printing raw HTML or arbitrary response fields. Structured streaming failures
also retain bounded code/parameter/reason/message details alongside recovery
advice. Before any shared UI/logging path, diagnostics redact the exact access
token used for that request and apply the existing redaction policy; response
headers other than the request ID are not exposed. HTTP authentication/policy
failures are not retried just because diagnostic text contains “timeout” or
“temporary”. Offline runtime fixtures cover HTTP 400/401/403/503, DNS/TLS/timeout
causes, request IDs, malformed bodies, token reflection and retry suppression.

## Concrete implementation sequence (future source changes)

1. Add native crypto/loopback/lock primitives in focused `src/` and `include/`
   modules; wire initialization in `src/plugins_runtime.c` or the existing native
   runtime initialization owner. Pin/build static crypto in `build.sh`/`Makefile`.
   Separate pure validation logic from Lua/UI adapters for unit testing.
2. Extend `agent/auth.lua` with account-aware storage/service APIs and safe
   projections; add the single SIWC policy module and thin plugin descriptor.
   Extend `plugins/connect.lua`, `plugins/logout.lua`, `plugins/auth.lua` for
   browser auth, account selection/status and revocation without breaking legacy
   plugin methods. Keep pending sign-in cancellable and UI-responsive.
3. Register provider defaults in `agent/provider_config.lua`; route canonical
   request/history adaptation through `agent/runtime.lua` and `agent/history.lua`.
   Add provider-native history persistence only at the actual session boundary;
   inspect that boundary before choosing its schema changes.
4. Extend `agent/models.lua` and its consumers with authenticated account-scoped
   catalogs; add terminal outcome/native item support in `agent/stream.lua`.
   Reuse `agent/tools.lua`, redaction and existing permission execution paths.
5. Embed new Lua assets in `Makefile`; update auth/config/session/stream specs,
   user migration examples and the AGENTS.md spec index in the implementation
   change. This design task owns only this document and does not edit the index.

## Required test matrix

| Area | Required positive and negative cases |
| --- | --- |
| Native crypto | Known SHA-256/PKCE and signature vectors; wrong signature/key/algorithm, `none`, malformed base64url/JSON, duplicate claims/keys, oversized inputs; entropy failure; UUIDv4 version/variant and restart stability |
| Loopback | Bound only to 127.0.0.1; dynamic port/exact redirect; wrong Host/path/method, duplicate params, bad encoding, missing/mismatched/replayed state, denial, timeout/cancel, occupied port; no browser before listener |
| Registration | Dynamic entrypoint versus issued exchange ID; missing issued ID; returning absent/equal/different callback ID; fresh PKCE/nonce every attempt; invalid-grant fresh flow; pending failure leaves active account intact |
| OIDC/JWKS | Wrong issuer/audience/azp/nonce/sub, expiry/skew/not-before; unknown kid rotates once; fetch failure and malicious JWKS URL; returning identity mismatch; scopes taken only from token response |
| Storage | 0700/0600, symlink/ownership rejection, corrupt state, atomic write/fsync failures, no executable credential loading, stable distinct records sharing email; no credential/hint URL leakage in logs/UI/telemetry |
| Concurrency | Two real processes refresh one rotating token once; reload after lock; cancellation/crash releases lock; failed persistence; refresh versus logout/account switch; no credential resurrection |
| Logout | Empty 200/already revoked, network/5xx backoff, explicit unconfirmed remote result, local write failure; mapping/host retained, all selected-account tokens removed, other accounts untouched |
| Catalog | Bearer auth/refresh, `models[]`, visibility/order/slug/display_name; account cache isolation/invalidation, malformed catalog, unknown model, no fake configured fallback |
| Request/history | `store=false`, `stream=true`, full input, no forbidden fields/previous_response_id/system items; namespace tool round-trip; multi-turn calls/results, opaque items, resume/compaction and provider migration rejection |
| Stream | Arbitrary byte fragmentation/CRLF/multiple events; late usage failure, incomplete/error/EOF without completed, terminal exactly once; tool fragments never execute before completed; safe retry and usage counters |
| Integration | TUI/CLI/ACP, roots/children/review/title/compaction, isolated runtime denial, private plugin coexistence, hook endpoint tampering, API-key provider regressions and legacy auth tests |
| Packaging | macOS arm64/x86_64 and Linux static crypto linkage, no new runtime dependency, embedded standalone smoke test and no host/account state in artifacts |

Use fixture keys and mocked HTTPS/token responses, not real home credentials.
Keep pure C tests in `make test`; dependency-aware native/Lua protocol tests in
an appropriate dedicated target or `make test-http-lua`; retain `make test-build`
for embedded assets. A live official-account acceptance test is opt-in and must
use explicitly supplied isolated state; no default browser/network sign-in in
CI. Tests above are requirements, not claims that they have been written/run.

## Open decisions and release blockers

- Approve/pin the static crypto library/version, licensing and build flags;
  verify actual discovery signing algorithms and JWKS endpoint origin policy.
- Confirm full official namespace tool and Responses history/reasoning schemas;
  obtain representative recorded, sanitized fixtures, not private-backend samples.
- Read the official token-reference/errors-and-recovery pages before implementing
  refresh omission/ID-token rules, expiry limits and recovery classification.
- Finalize cancellable native listener/lock API and account-picker command syntax
  within existing command contracts; do not introduce a parallel UI framework.
- Audit actual history persistence and redaction paths before shipping native
  output retention. Trusted plugins are not a security isolation boundary.
- Confirm preview availability/branding and perform explicit live acceptance;
  preview behavior can change. No compatibility with all ChatGPT accounts or
  models is implied by documentation alone.

## Sources

Official pages fetched for this design (preview contract; recheck before release):

- [Registration and sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in.md)
- [Models and inference](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference.md)
- [Accounts and sessions](https://developers.openai.com/siwc/token-sharing-open-source/profiles-and-sessions.md)
- [Preview limitations](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations.md)

Follow-up authoritative dependencies, linked by those pages but not inspected
for this design:

- [OIDC discovery](https://auth.openai.com/.well-known/openid-configuration)
- [Token reference](https://developers.openai.com/siwc/token-sharing-open-source/token-reference)
- [Errors and recovery](https://developers.openai.com/siwc/token-sharing-open-source/errors-and-recovery)
- [SIWC UI/UX guidelines](https://developers.openai.com/siwc/ui-ux-guidelines)

Local references: [OAuth auth](oauth-auth.md), [Config](config.md), and the
source/test seams listed above. No personal configuration, credential file or
private integration source was inspected for this design.
