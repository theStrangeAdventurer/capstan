#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p "$root/tmp"
home=$(mktemp -d "$root/tmp/start-screen.XXXXXX")
mkdir -p "$home/.config" "$home/.local/state"
# A separate home/state guarantees an empty session on every recording.
# Retain it under ignored tmp/ for diagnostics; never touch personal sessions.
cd "$home"
exec env -i HOME="$home" PATH="/usr/bin:/bin" TERM="${TERM:-xterm-256color}" \
  LANG=en_US.UTF-8 XDG_CONFIG_HOME="$home/.config" XDG_STATE_HOME="$home/.local/state" \
  "$root/build/capstan" --session-id start-screen-demo
