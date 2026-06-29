# Models Command

## Behavior

`/models<Tab>` opens a fuzzy-search popup listing models returned by every
configured provider's models API.

- Popup entries include the provider name and model label. They come from
  provider API responses, not from a static local list.
- Selecting a model from `/models<Tab>` sets the selected provider/model as an
  override for the current active profile.
- `/models <model-id>` changes the current active profile's model on the
  current provider.
- `/models <provider> <model-id>` changes a primary model for an explicit
  provider and makes that provider active.
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
- Successful selection updates the provider/model status line through
  `agent.set_info` when the effective active model changes.
- The command is a no-history control command: its result is shown as UI
  feedback but is not sent to the model and does not trigger an agent request.
- Selected primary models, profile models, and the weak model are persisted in
  [runtime state](runtime-state.md), not in `config.lua`.

## Provider API

`agent/models.lua` exposes `capstan.models` through the runtime:

- `list()` fetches and normalizes the current provider's models.
- `list_all()` fetches and normalizes models for all configured providers.
- `set(model_id)` updates and persists the current provider's model.
- `set_for(provider, model_id)` updates and persists an explicit provider's
  primary model.
- `weak()` returns the selected weak `{ provider, model }`, or `nil`.
- `set_weak(provider, model_id)` updates and persists the weak model.
- `profile(profile_name)` returns a selected profile model, or `nil`.
- `set_profile(profile_name, provider, model_id)` updates and persists a
  profile model.
- `effective(profile_name?)` reports the effective provider/model for a profile
  or the current active profile.
- `current_provider()` and `current_model()` report active runtime state.

For OpenAI-compatible providers, the models endpoint is derived from the chat
endpoint by replacing a trailing `/chat/completions` with `/models`. Providers
may define `models_endpoint` in `config.lua` when that convention is wrong.

## Tests

`make test-http-lua` covers provider API response normalization, `/models`
autocomplete entries, handler-driven runtime model selection, and selected-model
state persistence.
`make test-build` verifies the command is embedded in the standalone binary.
