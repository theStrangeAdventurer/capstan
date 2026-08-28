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

for command in /file /write /edit /shell /fetch /logs /skills /models /info /mcp /plan /implement /vcs; do
  printf '%s\n' "$output" | grep -q "plugin: $command" || {
    echo "missing embedded plugin command: $command" >&2
    exit 1
  }
done

printf '%s\n' "$output" | grep -q "self-improvement" && {
  echo "self-improvement skill loaded without explicit permission" >&2
  exit 1
}

trace_file="$rundir/smoke-trace.jsonl"
trace_stdout="$rundir/smoke-trace-stdout.json"
if trace_output=$(
  cd "$rundir"
  HOME="$homedir" "$bindir/capstan" run --prompt "trace smoke" \
    --provider smoke-missing --no-mcp --no-wiki --json \
    --trace-file "$trace_file" 2>/dev/null
); then
  echo "unknown provider unexpectedly succeeded in trace smoke" >&2
  exit 1
fi
printf '%s\n' "$trace_output" >"$trace_stdout"
python3 - "$trace_stdout" "$trace_file" <<'PY'
import json
import os
import stat
import sys

def require(condition, message):
    if not condition:
        raise SystemExit(message)

stdout_path, trace_path = sys.argv[1:]
result = json.loads(open(stdout_path, encoding="utf-8").read())
require(result["ok"] is False, "trace smoke JSON unexpectedly reports success")
require(result["text"] == "", "trace smoke JSON unexpectedly contains text")
require(isinstance(result["error"], str) and result["error"],
        "trace smoke JSON is missing its error")
events = [json.loads(line) for line in open(trace_path, encoding="utf-8") if line.strip()]
require(events and events[0]["event"] == "run.started",
        "trace smoke is missing its start event")
require(events[-1]["event"] == "run.finished",
        "trace smoke is missing its final event")
require(sum(event["event"] == "run.finished" for event in events) == 1,
        "trace smoke has multiple terminal events")
require(events[-1]["data"]["ok"] is False,
        "trace smoke terminal event unexpectedly reports success")
require(events[-1]["data"]["intended_exit_code"] == 1,
        "trace smoke terminal event has the wrong exit code")
require(stat.S_IMODE(os.stat(trace_path).st_mode) == 0o600,
        "trace smoke file permissions are not 0600")
require(not os.path.exists(trace_path + ".partial"),
        "trace smoke left a partial file after publication")
PY

if (
  cd "$rundir"
  HOME="$homedir" "$bindir/capstan" run --session-id "smoke session" \
    --prompt "first prompt" --provider smoke-missing --no-mcp --no-wiki \
    >/dev/null 2>&1
); then
  echo "unknown provider unexpectedly succeeded with new smoke session" >&2
  exit 1
fi
session_file=$(find "$homedir/.local/state/capstan/sessions" \
  -name 'smoke session.jsonl' -type f -print | head -n 1)
if [ -z "$session_file" ]; then
  echo "named smoke session was not persisted" >&2
  exit 1
fi
if (
  cd "$rundir"
  HOME="$homedir" "$bindir/capstan" run --session-id "smoke session" \
    --prompt "second prompt" --provider smoke-missing --no-mcp --no-wiki \
    >/dev/null 2>&1
); then
  echo "unknown provider unexpectedly succeeded while resuming smoke session" >&2
  exit 1
fi
[ "$(grep -c '\"role\":\"user\"' "$session_file")" -eq 2 ] || {
  echo "resumed smoke session did not preserve both prompts" >&2
  exit 1
}
grep -q 'first prompt' "$session_file" || {
  echo "resumed smoke session lost its original prompt" >&2
  exit 1
}
grep -q 'second prompt' "$session_file" || {
  echo "resumed smoke session did not persist its new prompt" >&2
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

if [ -e "$homedir/.local/state/capstan/builtin-skills" ]; then
  echo "built-in skills should not be materialized into runtime state" >&2
  exit 1
fi

printf '\nSmoke copy: %s\n' "$bindir/capstan"
printf 'Run manually from isolated dir:\n'
printf '  cd %s && HOME=%s %s/capstan\n' "$rundir" "$homedir" "$bindir"
