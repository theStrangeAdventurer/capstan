# Fetch Plugin

## Behavior

`/fetch <url>` fetches an HTTP or HTTPS URL and adds the response to the
conversation context.

- Missing URL returns `Usage: /fetch <url>`.
- URLs without a scheme are treated as HTTPS URLs when they look like a host,
  e.g. `example.com/docs` becomes `https://example.com/docs`.
- Non-HTTP(S) URLs are rejected with `Usage: /fetch <http-or-https-url>`.
- Successful responses display a compact UI summary:
  `Fetched <url> (HTTP <status>, <bytes> bytes)`.
- Non-2xx responses display `Fetch failed: <url> (HTTP <status>)`.
- HTTP redirects are followed by the shared HTTP layer, up to 10 redirects.
- Blocking HTTP responses are bounded by the shared HTTP layer's timeout and
  response-size limits.
- Requests send a `User-Agent` header:
  `Capstan/1.0 (+https://github.com/theStrangeAdventurer/tui-agent)`.
- Set `CAPSTAN_PLUGIN_FETCH_UA` to a non-empty value to override the default
  fetch `User-Agent`.
- The LLM-facing result includes the URL, HTTP status, and response body.

## Agent Tool

The plugin exposes an agent tool named `fetch`:

```lua
{
  name = "fetch",
  parameters = {
    type = "object",
    properties = {
      url = { type = "string" }
    },
    required = { "url" }
  }
}
```

The tool uses the same validation and response formatting as the slash command.
Agent-initiated fetches go through the normal [permission](permissions.md)
prompt path unless the user has granted a matching rule.

## Architecture

`plugins/fetch.lua` is a built-in Lua plugin. It uses the existing blocking
`http.get(url)` binding, so the UI spinner behavior is inherited from the HTTP
module. Redirect handling is configured in `src/http.c` so all Lua HTTP helpers
share the same redirect behavior. The shared HTTP layer also sets connection
and low-speed timeouts and rejects responses larger than the built-in response
limit instead of buffering unbounded data.

The plugin intentionally accepts only `http://` and `https://` URLs. Other
schemes are rejected so this command cannot bypass file access permissions via
libcurl-supported local schemes. The HTTP layer also restricts the initial URL
and redirect chain to HTTP(S).

## Tests

`make test-http-lua` runs Lua-level tests for the plugin without real network
access. The tests mock `http.get` and cover:

- plugin command/tool metadata
- missing URL validation
- non-HTTP(S) URL rejection
- successful slash-command fetch formatting
- default and environment-overridden `User-Agent` handling
- agent tool argument handling and non-2xx formatting

`make test-build` verifies `/fetch` is embedded into the standalone binary.
