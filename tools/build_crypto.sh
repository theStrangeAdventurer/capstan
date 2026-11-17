#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
VERSION=3.5.9
SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
case "$(uname -s)-$(uname -m)" in
    Darwin-*) echo 'macOS uses system cryptography; no OpenSSL build needed'; exit 0 ;;
    Linux-x86_64) TARGET=linux-x86_64 ;;
    Linux-aarch64|Linux-arm64) TARGET=linux-aarch64 ;;
    *) echo 'Unsupported OpenSSL target' >&2; exit 1 ;;
esac
SRC="$ROOT/vendor/openssl-$VERSION"
BUILD="$ROOT/vendor/openssl-build-$TARGET"
ARCHIVE="$SRC.tar.gz"
URL="https://github.com/openssl/openssl/releases/download/openssl-$VERSION/openssl-$VERSION.tar.gz"
mkdir -p vendor
if [ ! -f "$ARCHIVE" ]; then
    curl --fail --location --proto '=https' --tlsv1.2 "$URL" -o "$ARCHIVE.part"
    mv "$ARCHIVE.part" "$ARCHIVE"
fi
if command -v sha256sum >/dev/null; then
    ACTUAL=$(sha256sum "$ARCHIVE"); ACTUAL=${ACTUAL%% *}
else
    ACTUAL=$(shasum -a 256 "$ARCHIVE"); ACTUAL=${ACTUAL%% *}
fi
[ "$ACTUAL" = "$SHA256" ] || { echo 'OpenSSL checksum mismatch' >&2; exit 1; }
if [ ! -d "$SRC" ]; then tar -xzf "$ARCHIVE" -C vendor; fi
mkdir -p "$BUILD"
cd "$BUILD"
# Keep threads enabled; never load host configuration or dynamic providers.
perl "$SRC/Configure" "$TARGET" no-shared no-module no-dso no-engine \
    no-legacy no-apps no-tests no-docs no-autoload-config no-pinshared \
    --with-rand-seed=os -fvisibility=hidden
make -j"${JOBS:-4}" build_generated
make -j"${JOBS:-4}" libcrypto.a
printf '\nCrypto include flags: -I%s/include -I%s/include\n' "$BUILD" "$SRC"
printf 'Crypto archive: %s/libcrypto.a\n' "$BUILD"
# Link with -pthread. Linux: -Wl,--exclude-libs,libcrypto.a prevents libcurl
# interposition. macOS: hidden archive symbols above plus two-level namespace
# (default; do not use -flat_namespace or -export_dynamic) isolate libcrypto.
