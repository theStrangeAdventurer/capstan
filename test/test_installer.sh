#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${1:-"$repo_root/build/capstan"}

test_root=$(mktemp -d "${TMPDIR:-/tmp}/capstan-installer-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

release_root="$test_root/releases"
download_dir="$release_root/download/vtest"
install_dir="$test_root/bin"
mkdir -p "$download_dir"

sh "$repo_root/tools/package_release.sh" \
    "$binary" \
    "$download_dir/capstan-linux-x86_64.tar.gz" \
    capstan-linux-x86_64

if command -v sha256sum >/dev/null 2>&1; then
    (
        cd "$download_dir"
        sha256sum capstan-linux-x86_64.tar.gz >SHA256SUMS
    )
else
    (
        cd "$download_dir"
        shasum -a 256 capstan-linux-x86_64.tar.gz >SHA256SUMS
    )
fi

CAPSTAN_OS=linux \
CAPSTAN_ARCH=x86_64 \
CAPSTAN_VERSION=vtest \
CAPSTAN_RELEASES_URL="file://$release_root" \
CAPSTAN_INSTALL_DIR="$install_dir" \
    sh "$repo_root/install.sh"

test -x "$install_dir/capstan"
cmp "$binary" "$install_dir/capstan"

printf '%064d  %s\n' 0 capstan-linux-x86_64.tar.gz \
    >"$download_dir/SHA256SUMS"
if CAPSTAN_OS=linux \
   CAPSTAN_ARCH=x86_64 \
   CAPSTAN_VERSION=vtest \
   CAPSTAN_RELEASES_URL="file://$release_root" \
   CAPSTAN_INSTALL_DIR="$test_root/bad-bin" \
       sh "$repo_root/install.sh" >/dev/null 2>&1; then
    echo "installer accepted an invalid checksum" >&2
    exit 1
fi

if CAPSTAN_OS=windows \
   CAPSTAN_ARCH=x86_64 \
   CAPSTAN_VERSION=vtest \
   CAPSTAN_RELEASES_URL="file://$release_root" \
   CAPSTAN_INSTALL_DIR="$test_root/windows-bin" \
       sh "$repo_root/install.sh" >/dev/null 2>&1; then
    echo "installer accepted an unsupported operating system" >&2
    exit 1
fi
