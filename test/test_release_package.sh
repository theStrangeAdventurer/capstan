#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${1:-"$repo_root/build/capstan"}

test_root=$(mktemp -d "${TMPDIR:-/tmp}/capstan-package-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
archive="$test_root/capstan-test.tar.gz"

sh "$repo_root/tools/package_release.sh" "$binary" "$archive" capstan-test

actual="$test_root/actual.txt"
expected="$test_root/expected.txt"
tar -tzf "$archive" | sort >"$actual"
{
    printf '%s\n' \
        "capstan-test/" \
        "capstan-test/LICENSE" \
        "capstan-test/NOTICE" \
        "capstan-test/THIRD_PARTY_NOTICES" \
        "capstan-test/capstan"
} | sort >"$expected"
diff -u "$expected" "$actual"

tar -C "$test_root" -xzf "$archive"
test -x "$test_root/capstan-test/capstan"
cmp "$repo_root/LICENSE" "$test_root/capstan-test/LICENSE"
cmp "$repo_root/NOTICE" "$test_root/capstan-test/NOTICE"
cmp "$repo_root/THIRD_PARTY_NOTICES" \
    "$test_root/capstan-test/THIRD_PARTY_NOTICES"
