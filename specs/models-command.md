# Models Command

## Behavior

`/models<Tab>` opens a fuzzy-search popup listing models returned by every
configured provider's models API.

- Popup entries include the provider name and model label. Full catalogs come
  from provider API responses or an explicit provider `models` list. If neither
  source is available, the provider's configured `model` is still shown so
  every configured provider remains selectable.
- When the selected model advertises reasoning-effort support, selection opens
  a mandatory second popup. Its first item is `Default`, followed by the
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
- `/models --weak<Tab>` opens the same fuzzy-search popup, but selections are
  saved as the weak model.
- `/models --weak <provider> <model-id>` sets the weak model directly. The weak
  model is stored as both provider and model so background features such as
  compacting can use a cheaper model from a different provider. Weak model
  selection does not change the active provider.
- `/models --profile <fast|plan|implement> <provider> <model-id>` sets a
  provider/model override for that workflow profile. Plain `/models` targets
  the current active profile; explicit provider arguments target the global
  primary model.
- Successful selection updates the provider/model/reasoning status line through
  `agent.set_info` when the effective active model changes.
- The command is a no-history control command: its result is shown as UI
  feedback but is not sent to the model and does not trigger an agent request.
- Selected primary models, profile models, weak model, and their optional
  reasoning-effort choices are persisted in [runtime state](runtime-state.md),
  not in `config.lua`.

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
- `weak()` returns the selected weak `{ provider, model, reasoning_effort? }`,
  or `nil`.
- `set_weak(provider, model_id, reasoning_effort?)` updates and persists the
  weak model.
- `profile(profile_name)` returns a selected profile model, or `nil`.
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
model selection, and selected-model state persistence.
`make test-build` verifies the command is embedded in the standalone binary.
