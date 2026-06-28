# CI Binaries

Capstan publishes native build artifacts from GitHub Actions for the supported
desktop and Raspberry Pi-style targets.

## Targets

The `Build binaries` workflow builds three native binaries:

| Artifact | Runner | Purpose |
|----------|--------|---------|
| `capstan-macos-arm64` | `macos-14` | Apple Silicon macOS |
| `capstan-linux-x86_64` | `ubuntu-24.04` | Intel/AMD Linux |
| `capstan-linux-arm64` | `ubuntu-24.04-arm` | ARM Linux, including Raspberry Pi-class machines |

The workflow uses native runners rather than cross-compilation so vendored
ncurses and Lua are configured for the same OS and architecture as the final
binary.

## Build Behavior

Each job:

1. Installs Linux build dependencies when running on Ubuntu.
2. Runs `./build.sh`.
3. Runs `make test-build` against the produced standalone binary.
4. Uploads the renamed `build/capstan` binary as a GitHub Actions artifact.

On tag pushes matching `v*`, a follow-up release job downloads the three
artifacts and uploads them as assets on the matching GitHub Release. If the
release does not exist yet, the job creates it.

`libcurl` remains the only dynamic runtime dependency. ncurses and Lua are built
from vendored sources and linked as static archives.

## Constraints

The Linux ARM artifact is built on GitHub's ARM Ubuntu runner. A Raspberry Pi
manual build remains useful before public release because it verifies the
specific target device, kernel, distro packages, terminal database, and system
`libcurl` available to users.

Unit tests are not part of this artifact workflow while `vendor/munit` is a
gitlink without `.gitmodules`; `make test-build` remains the artifact gate.
