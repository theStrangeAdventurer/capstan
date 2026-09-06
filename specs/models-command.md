# Models Command

## Behavior

The start screen explicitly points users to `/models` as the interactive way to
choose a model. Typing `/models` and pressing Tab opens a fuzzy-search popup
listing models returned by every configured provider's models API.

- Popup entries include the provider name and model label. Full catalogs come
  from provider API responses or an explicit provider `models` list. If neither
  source is available, the provider's configured `model` is still shown so
  every configured provider remains selectable.
- When the selected model advertises reasoning-effort support and has no saved
  choice for this profile/provider/model, selection opens a second popup. Its first item is `Default`, followed by the
  efforts supported by that model (for example `Minimal`, `Low`, `Medium`,
  `High`, and `Max`). `Default` sends no effort override and leaves the choice
  to the provider/model.
- Models without reasoning-effort capability are selected immediately and do
  not show the second popup.
- Selecting a model from `/models<Tab>` sets the selected provider/model as an
  override for the current active profile.
- `/models <model-id>` changes the current active profile's model on the
  current provider.
- `/models <provider> <model-id>` changes a primary model for an explicit
  provider and makes that provider active. Direct command selection of a
  reasoning-capable model must add the effort as the next argument; `default`
  is an explicit valid choice.
- `/models --profile <profile> <provider> <model-id>` sets a provider/model
  override for that workflow profile. Plain `/models` targets
  the current active profile; explicit provider arguments target the global
  primary model.
- Successful selection updates the provider/model/reasoning status line through
  `agent.set_info` when the effective active model changes.
- The command is a no-history control command: its result is shown as UI
  feedback but is not sent to the model and does not trigger an agent request.
- Selected primary models, profile models, and their optional
  reasoning-effort choices are persisted in [runtime state](runtime-state.md),
  not in `config.lua`.

## Reasoning shortcuts

- `Shift-↑` raises effort and `Shift-↓` lowers it; `Shift-Tab` still cycles profiles.
- The ordered ladder is `default`, then the model-supported subset of `none`,
  `minimal`, `low`, `medium`, `high`, `xhigh`, `max`. Duplicate or unordered
  provider entries do not affect the ladder. Ends clamp instead of wrapping.
  An unsupported current level moves to the nearest supported level in the
  requested direction. `default` means no explicit effort override.
- Choices persist for each **profile + provider + model**, not just the active
  model of a profile. Switching A → B → A restores A's effort, including across
  restarts. Other profiles and providers (even with identical model ids) are
  independent. The provider/model stays unchanged when stepping effort.
- `/models` skips the effort popup for a remembered tuple. Direct profile model
  selection without an effort restores that tuple, or uses `default` if absent.
  An explicit effort (including `default`) replaces only that tuple's choice.
  Primary model selection retains its existing explicit-effort behavior.
- Restored efforts are checked against static/declared/cached capabilities. An
  unsupported or unknown saved level resolves to `default`, never another model's
  choice or a config/profile effort. No catalog fetch is needed for restoration.
  `agent.models` owns this policy; state stores history, and the command is an
  adapter. The active profile selection remains a compatibility snapshot.
- The footer/start screen update immediately, with no chat message or request.
- The Lua runtime owns effective selection and persistence; C only decodes keys
  and calls `capstan.agent.step_reasoning_effort(1|-1)`. An interactive adapter
  lets a manual choice override launch-time effort for the matching profile and
  model without altering launch routing or explicit options of API/CLI runs.
- Only static, declared, or already-cached capabilities are used. A successful
  `/models` fetch feeds that cache. Keys never fetch catalogs or make API calls.
  Unknown/unsupported capabilities show a short notification suggesting `/models`;
  settings are unchanged. Persistence errors are shown explicitly; an in-memory
  change is retained and displayed even if saving fails.
- Works in input/message focus and blocking wait frames. Open popups/permission
  dialogs retain their keyboard ownership. Bracketed paste remains literal.
  Standard xterm `CSI 1;2A/B` sequences are registered explicitly as ncurses
  `KEY_SR/SF`; terminals must forward these keys rather than intercept them.
- Changes apply to the next request/run, not an in-flight run's captured effort
  (including its tool continuations). Inherited background model options follow
  the new choice; explicit background/subagent options remain independent.

## Provider API

`agent/models.lua` exposes `capstan.models` through the runtime:

- `list()` fetches and normalizes the current provider's models.
- `list_all()` fetches and normalizes models for all configured providers.
- `reasoning_efforts(provider, model_id)` reports the normalized effort list.
  A provider's explicit `default_reasoning_efforts` declaration applies when
  its models endpoint omits OpenAI-style `supported_parameters` metadata. When
  `supported_parameters` is present, that list is authoritative: defaults apply
  only if it includes `reasoning` or `reasoning_effort`. A provider's explicit
  per-model `reasoning_efforts` override remains authoritative.
- `set(model_id, reasoning_effort?)` updates and persists the current
  provider's model.
- `set_for(provider, model_id, reasoning_effort?)` updates and persists an
  explicit provider's primary model.
- `saved_profile_effort(profile, provider, model)` returns a capability-checked
  remembered choice (`default` on unsupported), or `nil` if absent.
- `profile(profile_name)` returns a selected profile model with restored effort,
  or `nil`.
- `set_profile(profile_name, provider, model_id, reasoning_effort?)` updates
  and persists a profile model.
- `effective(profile_name?)` reports the effective provider/model and reasoning
  effort for a profile or the current active profile.
- `current_provider()` and `current_model()` report active runtime state.

For OpenAI-compatible providers, the models endpoint is derived from the chat
endpoint by replacing a trailing `/chat/completions` with `/models`. Providers
may define `models_endpoint` in `config.lua` when that convention is wrong.

## Tests

`make test-http-lua` covers provider API response normalization, the mandatory
reasoning-effort drill-down, direct-command enforcement, handler-driven runtime
model selection, and selected-model state persistence. `test_reasoning_shortcuts.lua`
adds ladder, cache, profile isolation, launch override, request snapshot, and save
failure coverage. `test_reasoning_memory.lua` covers A/B/A restoration, provider
and profile isolation, restart, legacy migration, explicit reset, command/popup
selection, changed capabilities, request precedence, and save failures.
`make test-tui-input` checks actual Shift-arrow decoding,
profile switching, footer updates, blocking waits, and preservation of drafts/paste.
`make test-build` verifies the command is embedded in the standalone binary.
