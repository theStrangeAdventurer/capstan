#!/usr/bin/env bash
set -euo pipefail

PINNED_COMMIT="7e0611e77b54e2dea774cdc0aa00cf9f7ed6144f"
CORPUS_URL="https://github.com/Aider-AI/polyglot-benchmark.git"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CORPUS="$ROOT/benchmarks/work/polyglot-benchmark"
CHECK_ONLY=0

usage() {
  cat <<'EOF'
Usage: benchmarks/polyglot/bootstrap.sh [--corpus DIR] [--check]

Clone Aider Polyglot at the commit required by the mini-v2 suite and check the
language toolchains used by its 12 tasks. The corpus is deliberately kept out
of Git under benchmarks/work/.

Options:
  --corpus DIR  Destination or existing corpus checkout
  --check       Do not clone; validate the checkout and toolchains only
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --corpus)
      [ "$#" -ge 2 ] || { echo "--corpus requires a path" >&2; exit 2; }
      CORPUS="$2"
      shift 2
      ;;
    --check) CHECK_ONLY=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

need_tools=(git python3 cmake go java node npm cargo)
missing=()
for tool in "${need_tools[@]}"; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ "${#missing[@]}" -ne 0 ]; then
  printf 'missing required tools: %s\n' "${missing[*]}" >&2
  exit 1
fi

if [ ! -e "$CORPUS/.git" ]; then
  if [ "$CHECK_ONLY" -eq 1 ]; then
    printf 'corpus checkout does not exist: %s\n' "$CORPUS" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$CORPUS")"
  git clone "$CORPUS_URL" "$CORPUS"
fi

status="$(git -C "$CORPUS" status --short)"
if [ -n "$status" ]; then
  printf 'corpus checkout is dirty: %s\n' "$CORPUS" >&2
  exit 1
fi

git -C "$CORPUS" fetch --quiet --tags origin
if ! git -C "$CORPUS" cat-file -e "${PINNED_COMMIT}^{commit}" 2>/dev/null; then
  printf 'pinned corpus commit is unavailable: %s\n' "$PINNED_COMMIT" >&2
  exit 1
fi
git -C "$CORPUS" checkout --quiet --detach "$PINNED_COMMIT"

echo "Corpus ready: $CORPUS"
echo "Commit: $(git -C "$CORPUS" rev-parse HEAD)"
echo "Toolchains: ${need_tools[*]}"
