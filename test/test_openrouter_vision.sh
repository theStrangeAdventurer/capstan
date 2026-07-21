#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture=${1:-"$repo_root/test/fixtures/vision-shapes.png"}
model=${OPENROUTER_VISION_MODEL:-minimax/minimax-m3}

if [ -z "${OPENROUTER_API_KEY:-}" ]; then
  echo "OPENROUTER_API_KEY is required for the live vision smoke test" >&2
  exit 2
fi

case "$model" in
  *[!A-Za-z0-9._:/-]*)
    echo "invalid OPENROUTER_VISION_MODEL: $model" >&2
    exit 2
    ;;
esac

if [ ! -f "$fixture" ]; then
  echo "missing vision fixture: $fixture" >&2
  exit 2
fi

request=$(mktemp "${TMPDIR:-/tmp}/capstan-openrouter-vision-request.XXXXXX")
response=$(mktemp "${TMPDIR:-/tmp}/capstan-openrouter-vision-response.XXXXXX")
trap 'rm -f "$request" "$response"' EXIT HUP INT TERM

printf '%s' \
  '{"model":"'"$model"'","stream":false,"messages":[{"role":"user","content":[{"type":"text","text":"Inspect the image. If and only if it contains a blue square on the left and a red triangle on the right, reply exactly CAPSTAN_VISION_OK. Otherwise describe the mismatch."},{"type":"image_url","image_url":{"url":"data:image/png;base64,' \
  >"$request"
base64 <"$fixture" | tr -d '\r\n' >>"$request"
printf '%s' '"}}]}]}' >>"$request"

http_code=$(
  curl -sS \
    -o "$response" \
    -w '%{http_code}' \
    https://openrouter.ai/api/v1/chat/completions \
    -H "Authorization: Bearer $OPENROUTER_API_KEY" \
    -H 'Content-Type: application/json' \
    --data-binary "@$request"
)

if [ "$http_code" != "200" ]; then
  echo "OpenRouter vision smoke failed: HTTP $http_code" >&2
  sed -n '1,20p' "$response" >&2
  exit 1
fi

if ! grep -q 'CAPSTAN_VISION_OK' "$response"; then
  echo "OpenRouter accepted the image but the model did not identify the expected shapes" >&2
  sed -n '1,20p' "$response" >&2
  exit 1
fi

printf 'OpenRouter vision smoke passed: model=%s fixture=%s\n' "$model" "$fixture"
