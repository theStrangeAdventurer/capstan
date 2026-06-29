# Info Command

## Behavior

`/info` shows a popup with local runtime information. It is a control command:
the entered command and popup content are not added to conversation history and
are not sent to the model.

The popup uses the message-popup renderer, expands to the available terminal
area when content is tall, and supports scrolling with `j`/`k`, arrow keys,
Ctrl-D/Ctrl-U, `g`, and `G`. Enter or Escape closes it.

The popup includes:

- current workspace directory
- config directory and important config paths
- skill directories and embedded skill sources loaded by the runtime
- state directory, runtime state, permissions file, and current log file
- active profile, current provider/model, weak provider/model, profile-specific
  model configuration, the model each profile uses after fallback resolution,
  and the effective reasoning effort for each profile when the model runtime is
  available

The empty conversation screen stays minimal and does not duplicate this
diagnostic information.

## Architecture

`plugins/info.lua` sets `plugin.history = false`, so the dispatcher routes the
result to `popup_show_message()` instead of buffering it as context.

The plugin reads paths through the `capstan` runtime table. C exposes:

- `capstan.config_dir()`
- `capstan.config_path(relative)`
- `capstan.state_dir()`
- `capstan.state_path(relative)`
- `capstan.log_path()`
- `capstan.workdir`

The plugin reads model status through `capstan.models` and profile effort
through `capstan.agent.reasoning_effort(profile)`. In the popup, `configured`
means the profile has an explicit provider/model override. `uses` means the
provider/model that profile will actually use after profile, global, runtime
state, and environment fallbacks are resolved.

## Tests

`make test-http-lua` verifies that `/info` is popup-only and includes the
runtime paths/model details supplied by the host runtime.

`make test` verifies that message popups retain scroll state for keyboard
navigation and reset it when closed.

`make test-build` verifies that `/info` is embedded in the standalone binary.
