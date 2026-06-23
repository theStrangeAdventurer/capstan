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

for command in /file /write /edit /shell /fetch /logs /skills /models; do
  printf '%s\n' "$output" | grep -q "plugin: $command" || {
    echo "missing embedded plugin command: $command" >&2
    exit 1
  }
done

printf '%s\n' "$output" | grep -q "self-improvement" && {
  echo "self-improvement skill loaded without explicit permission" >&2
  exit 1
}

mkdir -p "$homedir/.config/capstan"
cat >"$homedir/.config/capstan/config.lua" <<'EOF'
return {
  capabilities = {
    self_improvement = true,
  },
}
EOF

enabled_output=$(
  cd "$rundir"
  HOME="$homedir" "$bindir/capstan" --self-test-embedded
)

printf '%s\n' "$enabled_output"
printf '%s\n' "$enabled_output" | grep -q -- "- self-improvement \\[builtin\\]" || {
  echo "missing gated built-in self-improvement skill" >&2
  exit 1
}

printf '\nSmoke copy: %s\n' "$bindir/capstan"
printf 'Run manually from isolated dir:\n'
printf '  cd %s && HOME=%s %s/capstan\n' "$rundir" "$homedir" "$bindir"
