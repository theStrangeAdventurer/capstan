# Self Improvement

Capstan has a built-in `self-improvement` skill for tasks where the agent needs
to extend Capstan itself with user plugins, tools, slash commands, autocomplete,
or hooks.

## Permission

The skill is unavailable by default. The user must explicitly enable it in the
home config:

```lua
return {
  capabilities = {
    self_improvement = true,
  },
}
```

Only `capabilities.self_improvement = true` enables the skill. Missing config,
missing section, or any other value keeps it disabled.

## Behavior

- The skill is embedded into the binary as `skills/self-improvement/SKILL.md`.
- At startup, after `config.lua` is loaded, Capstan checks the permission flag.
- If allowed, Capstan loads the embedded skill directly from memory as:

```text
embedded:skills/self-improvement/SKILL.md
```

- The in-memory skill is scanned as skill source `builtin`; no `SKILL.md` copy
  is written into the state directory.
- Startup removes the legacy materialized
  `builtin-skills/self-improvement/SKILL.md` state file from older Capstan
  versions, then removes the generated directories only if they are empty.
- Source priority still allows user and project skills to override the built-in
  `self-improvement` skill by defining the same skill name.
- If permission is disabled, the skill is not loaded and does not appear in the
  system prompt skill index or `/skills`.

## Scope

The skill tells the agent to prefer durable Lua extensions under:

```text
~/.config/capstan/plugins/*.lua
```

Those files are watched and hot-reloaded by the running process. The skill is
for extensions that are necessary to solve the user task; ordinary one-off work
should not create persistent plugins or hooks.

## Tests

`make test` covers built-in skill source loading and override priority.
`make test-build` verifies the embedded asset is present in the standalone
binary build path.
