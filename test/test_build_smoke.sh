#!/bin/sh
set -eu

binary=${1:-build/capstan}

if [ ! -x "$binary" ]; then
  echo "missing executable binary: $binary" >&2
  exit 1
fi

workdir=$(mktemp -d "${TMPDIR:-/tmp}/capstan-build-smoke.XXXXXX")
bindir="$workdir/bin"
homedir="$workdir/home"
rundir="$workdir/run"
mkdir -p "$bindir" "$homedir" "$rundir"

cp "$binary" "$bindir/capstan"
chmod +x "$bindir/capstan"

output=$(
  cd "$rundir"
  HOME="$homedir" "$bindir/capstan" --self-test-embedded
)

printf '%s\n' "$output"

for command in /file /write /shell /fetch /logs; do
  printf '%s\n' "$output" | grep -q "plugin: $command" || {
    echo "missing embedded plugin command: $command" >&2
    exit 1
  }
done

printf '\nSmoke copy: %s\n' "$bindir/capstan"
printf 'Run manually from isolated dir:\n'
printf '  cd %s && HOME=%s %s/capstan\n' "$rundir" "$homedir" "$bindir"
