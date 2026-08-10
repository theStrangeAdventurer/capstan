#!/usr/bin/env bash
set -euo pipefail

PROMPTFOO_VERSION="0.121.19"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG=""
RESULTS_ROOT="${BENCHMARK_RESULTS_ROOT:-$PWD/benchmarks/results/promptfoo}"

usage() {
  cat <<'EOF'
Usage: benchmarks/polyglot/promptfoo/run.sh --config FILE [--out DIR] [promptfoo args...]

Run the optional Promptfoo presentation layer around the canonical Polyglot
harness. Create FILE from config.example.yaml and keep it outside Git.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --config)
      [ "$#" -ge 2 ] || { echo "--config requires a file" >&2; exit 2; }
      CONFIG="$2"
      shift 2
      ;;
    --out)
      [ "$#" -ge 2 ] || { echo "--out requires a directory" >&2; exit 2; }
      RESULTS_ROOT="$2"
      shift 2
      ;;
    --help|-h) usage; exit 0 ;;
    *) break ;;
  esac
done

[ -n "$CONFIG" ] || { echo "--config is required" >&2; usage >&2; exit 2; }
[ -f "$CONFIG" ] || { echo "config not found: $CONFIG" >&2; exit 2; }
CONFIG="$(cd "$(dirname "$CONFIG")" && pwd)/$(basename "$CONFIG")"
case "$CONFIG" in
  "$SCRIPT_DIR"/*) ;;
  *)
    echo "config must live under $SCRIPT_DIR so Promptfoo can resolve its adapters" >&2
    exit 2
    ;;
esac
command -v npx >/dev/null 2>&1 || { echo "npx is required" >&2; exit 1; }

run_id="$(date -u +%Y%m%d-%H%M%S)"
run_dir="$RESULTS_ROOT/$run_id"
state_dir="$run_dir/promptfoo-state"
mkdir -p "$run_dir" "$state_dir"

export PROMPTFOO_CONFIG_DIR="$state_dir"
export PROMPTFOO_DISABLE_TELEMETRY=1
export PROMPTFOO_PYTHON="${PROMPTFOO_PYTHON:-python3}"

npx --yes "promptfoo@${PROMPTFOO_VERSION}" eval \
  --config "$CONFIG" \
  --no-cache \
  --output "$run_dir/report.html" \
  --output "$run_dir/results.json" \
  "$@"

echo "HTML report: $run_dir/report.html"
echo "JSON results: $run_dir/results.json"
echo "Interactive viewer: PROMPTFOO_CONFIG_DIR=$state_dir npx --yes promptfoo@$PROMPTFOO_VERSION view $state_dir"
