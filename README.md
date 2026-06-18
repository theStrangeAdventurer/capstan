# tui-agent

CLI LLM agent — like opencode / claude code.

## Build

```sh
./build.sh
```

The script handles everything: checks dependencies, builds ncurses + Lua from `vendor/`, then compiles the project.

Binary is `build/capstan`.

## System dependencies

### macOS
- Xcode Command Line Tools (`xcode-select --install`)
- libcurl (included in macOS SDK)

### Linux (Debian/Ubuntu)
```sh
sudo apt install build-essential libcurl4-openssl-dev
```

### Linux (Fedora)
```sh
sudo dnf install gcc make libcurl-devel
```

## Run

```sh
./build/capstan
```

Requires `DEEPSEEK_API_KEY` or `OPENAI_API_KEY` environment variable.

## Project structure

```
src/        C sources (single gcc invocation)
include/    headers
ai/         Lua AI core (providers, tool calls)
plugins/    Lua plugins (commands: /file, /shell, /write, etc.)
vendor/     third-party: ncurses + Lua source, rxi/json.lua
```
