# Models Command

## Behavior

`/models<Tab>` opens a popup listing models returned by the current provider's
models API.

- The current provider is `capstan.models.current_provider()`, backed by the
  `agent/` Lua runtime.
- The popup entries come from the provider API response, not from a static local
  list.
- Selecting a model runs `/models <model-id>` and changes the model for the
  current runtime session.
- Successful selection updates the provider/model status line through
  `agent.set_info`.
- The command is a no-history control command: its result is shown as UI
  feedback but is not sent to the model and does not trigger an agent request.
- The selected model is persisted in [runtime state](runtime-state.md), not in
  `config.lua`.

## Provider API

`agent/models.lua` exposes `capstan.models` through the runtime:

- `list()` fetches and normalizes the current provider's models.
- `set(model_id)` updates and persists the current provider's model.
- `current_provider()` and `current_model()` report active runtime state.

For OpenAI-compatible providers, the models endpoint is derived from the chat
endpoint by replacing a trailing `/chat/completions` with `/models`. Providers
may define `models_endpoint` in `config.lua` when that convention is wrong.

## Tests

`make test-http-lua` covers provider API response normalization, `/models`
autocomplete entries, handler-driven runtime model selection, and selected-model
state persistence.
`make test-build` verifies the command is embedded in the standalone binary.
