# Runtime State

Runtime state stores user choices made by the running application. It is separate
from editable config defaults.

## Paths

Capstan stores runtime state in the XDG state directory:

```text
$XDG_STATE_HOME/capstan/state.lua
```

When `XDG_STATE_HOME` is not set, the fallback is:

```text
~/.local/state/capstan/state.lua
```

The runtime exposes these helpers to Lua:

- `capstan.state`: loaded state table, or `{}` when no state exists.
- `capstan.state_path(relative_path)`: path inside the state directory.
- `capstan.state_ensure_dir()`: creates the state directory.

## Behavior

`config.lua` is the editable source of defaults. Runtime choices should not
rewrite `config.lua`.

Currently persisted runtime state includes selected provider models:

```lua
return {
  models = {
    openrouter = "openai/gpt-4.1",
  },
}
```

Model precedence is:

1. environment model override;
2. persisted runtime state;
3. `config.lua`;
4. built-in provider default.

Permission prompt choices made with `Always allow` are stored separately in:

```text
$XDG_STATE_HOME/capstan/permissions.lua
```

or `~/.local/state/capstan/permissions.lua` when `XDG_STATE_HOME` is unset.
Declarative permission defaults belong in `~/.config/capstan/config.lua` under
the `permissions` key.

## Tests

`make test` covers XDG state path selection. `make test-http-lua` covers
persisting selected models and applying state during provider runtime startup.
