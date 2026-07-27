#!/bin/sh
set -eu

REPOSITORY=theStrangeAdventurer/capstan
RELEASES_URL=${CAPSTAN_RELEASES_URL:-"https://github.com/$REPOSITORY/releases"}
INSTALL_DIR=${CAPSTAN_INSTALL_DIR:-"$HOME/.local/bin"}

fail() {
    echo "capstan installer: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        fail "required command not found: $1"
}

require_command curl
require_command tar

raw_os=${CAPSTAN_OS:-$(uname -s)}
raw_arch=${CAPSTAN_ARCH:-$(uname -m)}

case "$raw_os" in
    Darwin|darwin|macOS|macos)
        platform=macos
        ;;
    Linux|linux)
        platform=linux
        ;;
    *)
        fail "unsupported operating system: $raw_os"
        ;;
esac

case "$raw_arch" in
    arm64|aarch64)
        architecture=arm64
        ;;
    x86_64|amd64)
        architecture=x86_64
        ;;
    *)
        fail "unsupported architecture: $raw_arch"
        ;;
esac

if [ "$platform" = "macos" ] && [ "$architecture" = "x86_64" ]; then
    fail "macOS x86_64 binaries are not available yet; build Capstan from source"
fi

version=${CAPSTAN_VERSION:-}
if [ -z "$version" ]; then
    latest_url=$(curl -fsSL -o /dev/null -w '%{url_effective}' \
        "$RELEASES_URL/latest") ||
        fail "could not resolve the latest release"
    case "$latest_url" in
        */tag/*)
            version=${latest_url##*/}
            ;;
        *)
            fail "unexpected latest-release URL: $latest_url"
            ;;
    esac
fi

package_name="capstan-$platform-$architecture"
archive_name="$package_name.tar.gz"
download_url="$RELEASES_URL/download/$version"

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/capstan-install.XXXXXX")
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM

echo "Downloading Capstan $version for $platform/$architecture..."
curl -fsSL "$download_url/$archive_name" \
    -o "$temporary_dir/$archive_name" ||
    fail "could not download $archive_name"
curl -fsSL "$download_url/SHA256SUMS" \
    -o "$temporary_dir/SHA256SUMS" ||
    fail "could not download SHA256SUMS"

expected_checksum=$(awk -v name="$archive_name" '$2 == name { print $1 }' \
    "$temporary_dir/SHA256SUMS")
[ -n "$expected_checksum" ] ||
    fail "SHA256SUMS does not contain $archive_name"

if command -v sha256sum >/dev/null 2>&1; then
    actual_checksum=$(sha256sum "$temporary_dir/$archive_name" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual_checksum=$(shasum -a 256 "$temporary_dir/$archive_name" | awk '{print $1}')
else
    fail "sha256sum or shasum is required to verify the download"
fi

[ "$actual_checksum" = "$expected_checksum" ] ||
    fail "checksum verification failed for $archive_name"

tar -C "$temporary_dir" -xzf "$temporary_dir/$archive_name"
binary="$temporary_dir/$package_name/capstan"
[ -f "$binary" ] || fail "release archive does not contain $package_name/capstan"

mkdir -p "$INSTALL_DIR"
cp "$binary" "$INSTALL_DIR/capstan"
chmod +x "$INSTALL_DIR/capstan"

echo "Installed Capstan $version to $INSTALL_DIR/capstan"
case ":${PATH:-}:" in
    *":$INSTALL_DIR:"*)
        ;;
    *)
        echo "Add $INSTALL_DIR to PATH, for example:"
        echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
        ;;
esac
