# OAuth Auth

Capstan has an OAuth credential path for trusted local provider plugins. The
core feature is provider-neutral: API-key provider behavior remains unchanged,
and bundled public providers do not depend on OAuth.

## Storage

OAuth credentials are stored in:

```text
$XDG_STATE_HOME/capstan/state/auth.lua
```

or `~/.local/state/capstan/state/auth.lua` when `XDG_STATE_HOME` is unset.

Lua exposes `capstan.auth` through `agent/auth.lua`:

```lua
capstan.auth.get(provider_id)
capstan.auth.set(provider_id, credential)
capstan.auth.remove(provider_id)
capstan.auth.list()
capstan.auth.redacted(provider_id)
```

Stored credentials use:

```lua
{
  type = "oauth",
  access = "...",
  refresh = "...",
  expires = 1790000000000,
  metadata = {},
}
```

`auth.lua` is written through Capstan's secure file writer, which creates a
temporary file with mode `0600` before writing token contents and then renames it
into place.

## Commands

- `/connect` lists OAuth-capable providers.
- `/connect <provider>` runs the provider's default OAuth method.
- `/logout <provider>` removes that provider's OAuth credential.
- `/auth` shows redacted credential status.

## Out-of-tree Provider Plugins

OAuth providers that rely on private, experimental, or non-public integration
details must live outside the public Capstan source tree as user plugins, for
example under `~/.config/capstan/plugins/*.lua` or symlinked there from a
private repository.

At request time, an OAuth provider plugin normally uses a `before_request` hook
to:

- require a stored OAuth credential for its provider id;
- refresh expired credentials and persist the refreshed value;
- add provider-specific authorization headers;
- rewrite endpoint/body shape only when the provider's API requires it;
- abort the request with a visible error when auth is missing or refresh fails.

## Tests

`make test-http-lua` covers credential set/get/remove and redaction. `make
test-build` verifies the auth commands are embedded in standalone binaries.
