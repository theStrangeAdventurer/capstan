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

Currently persisted runtime state includes the active provider, selected
provider models, optional profile models, the
reasoning-effort choice made with each model selection, and the VCS adapter
selected for each canonical workspace root:

```lua
return {
  provider = "openrouter",
  models = {
    openrouter = "openai/gpt-4.1",
  },
  model_reasoning_efforts = {
    openrouter = "default",
  },
  profile_reasoning_efforts = {
    plan = {
      openrouter = {
        ["anthropic/claude-sonnet-4"] = "medium",
        ["openai/gpt-4.1"] = "default",
      },
    },
  },
  vcs_by_workspace = {
    ["/repo"] = "git",
  },
  profile_models = {
    plan = {
      provider = "openrouter",
      model = "anthropic/claude-sonnet-4",
      reasoning_effort = "medium",
    },
  },
}
```

Provider precedence is:

1. environment provider override;
2. persisted runtime state;
3. `config.lua`;
4. built-in provider default.

Model precedence is:

1. environment model override;
2. persisted runtime state;
3. `config.lua`;
4. built-in provider default.

For a reasoning-capable model, a persisted effort selected alongside that
model takes precedence over config and profile defaults. The literal
`"default"` intentionally suppresses those overrides so the provider/model
chooses its own default.

`profile_reasoning_efforts[profile][provider][model]` retains each profile's
choices even when selecting a different model/provider. `profile_models` still
stores active routing and an effort snapshot for compatibility. The history is
authoritative when present; legacy snapshots are read only for their exact tuple
and copied into history before replacing the active selection. Migration needs no
startup write. Explicit `default` is stored as a value, not removed. On restore,
`agent.models` checks capabilities locally and uses `default` for an unsupported
saved effort. Config defaults and primary-model state retain their existing roles.

Legacy `weak_model` fields in config or loaded state are ignored and are not
written back when runtime state is saved.

Profile model precedence for an active profile is:

1. explicit run provider/model options such as `--provider` and `--model`;
2. environment provider/model overrides;
3. persisted runtime state `profile_models[profile]`;
4. `config.lua` `agent.profile_models[profile]`;
5. the selected global primary provider/model.

Session permission grants are in-memory only and are not runtime state.
Explicit workflows such as `wiki_ingest`, plus rules retained from older
Capstan versions, may use the separate
`$XDG_STATE_HOME/capstan/permissions.lua` file (or
`~/.local/state/capstan/permissions.lua`). Declarative permanent permission
rules belong in `~/.config/capstan/config.lua` under the `permissions` key.

## Tests

`make test` covers XDG state path selection. `make test-http-lua` covers
persisting selected primary and profile models, ignoring legacy fields, and
applying state during provider runtime startup.
