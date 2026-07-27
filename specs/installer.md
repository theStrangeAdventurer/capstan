# Installer

Capstan provides a POSIX shell installer at the repository root:

```sh
curl -fsSL https://raw.githubusercontent.com/theStrangeAdventurer/capstan/main/install.sh | sh
```

## Behavior

The installer:

1. Detects macOS or Linux through `uname -s`.
2. Detects `arm64`/`aarch64` or `x86_64`/`amd64` through `uname -m`.
3. Resolves the latest GitHub Release unless `CAPSTAN_VERSION` is set.
4. Downloads the matching `.tar.gz` and the release `SHA256SUMS`.
5. Verifies the archive before extracting it.
6. Installs the executable into `~/.local/bin` by default.
7. Prints a `PATH` hint when the install directory is not currently reachable.

Supported prebuilt targets are:

| OS | Architecture | Asset |
|----|--------------|-------|
| macOS | arm64 | `capstan-macos-arm64.tar.gz` |
| Linux | x86_64 | `capstan-linux-x86_64.tar.gz` |
| Linux | arm64 | `capstan-linux-arm64.tar.gz` |

Unsupported platforms fail closed with a source-build recommendation.

## Configuration

- `CAPSTAN_VERSION` selects a release such as `v0.1.0`.
- `CAPSTAN_INSTALL_DIR` changes the destination directory.
- `CAPSTAN_RELEASES_URL` overrides the GitHub Releases base URL for tests or
  mirrors.
- `CAPSTAN_OS` and `CAPSTAN_ARCH` override platform detection for deterministic
  tests.

## Release Contract

Each platform archive contains one top-level directory named after the target,
with:

```text
capstan
LICENSE
NOTICE
THIRD_PARTY_NOTICES
```

Every published release must include a `SHA256SUMS` file generated from the
archives. The installer refuses missing or mismatched checksums.

## Tests

`test/test_installer.sh` builds a local release layout, installs through a
`file://` URL, compares the installed executable byte-for-byte, and verifies
that bad checksums and unsupported operating systems are rejected.

`make test-build` runs the installer test after the embedded-runtime and release
package smoke tests.
