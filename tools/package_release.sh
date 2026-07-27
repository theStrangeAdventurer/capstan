#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <binary> <archive.tar.gz> <package-name>" >&2
    exit 2
fi

binary=$1
archive=$2
package_name=$3
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

for required in "$binary" "$repo_root/LICENSE" "$repo_root/NOTICE" \
                "$repo_root/THIRD_PARTY_NOTICES"; do
    if [ ! -f "$required" ]; then
        echo "missing release file: $required" >&2
        exit 1
    fi
done

case "$package_name" in
    ""|*/*|.|..)
        echo "invalid package name: $package_name" >&2
        exit 2
        ;;
esac

archive_dir=$(dirname -- "$archive")
archive_name=$(basename -- "$archive")
mkdir -p "$archive_dir"
archive_dir=$(CDPATH= cd -- "$archive_dir" && pwd)

stage_root=$(mktemp -d "${TMPDIR:-/tmp}/capstan-package.XXXXXX")
trap 'rm -rf "$stage_root"' EXIT HUP INT TERM
package_dir="$stage_root/$package_name"
mkdir -p "$package_dir"

cp "$binary" "$package_dir/capstan"
cp "$repo_root/LICENSE" "$repo_root/NOTICE" "$repo_root/THIRD_PARTY_NOTICES" \
   "$package_dir/"
chmod +x "$package_dir/capstan"

tar -C "$stage_root" -czf "$archive_dir/$archive_name" "$package_name"
