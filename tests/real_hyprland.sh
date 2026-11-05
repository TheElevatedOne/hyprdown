#!/usr/bin/env bash
# Runs hyprdown against a real headless Hyprland. Usage: real_hyprland.sh BINARY
# GitHub Actions only: this must never run inside somebody's session.
set -euo pipefail

[ "${GITHUB_ACTIONS:-}" = "true" ] || { echo "refusing to run outside GitHub Actions" >&2; exit 2; }

binary=$(realpath "$1")
work=$(mktemp -d)
export XDG_RUNTIME_DIR="$work/run" XDG_CONFIG_HOME="$work/config" XDG_STATE_HOME="$work/state" HOME="$work/home"
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$HOME"
echo 'hl.config({ misc = { disable_hyprland_logo = true } })' > "$work/test.lua"

HYPRLAND_HEADLESS_ONLY=1 Hyprland --config "$work/test.lua" > "$work/hyprland.out" 2>&1 &
hyprland=$!
trap 'kill -9 "$hyprland" 2>/dev/null || true; tail -30 "$work/hyprland.out" || true; tail -30 "$XDG_STATE_HOME/hyprdown/hyprdown.log" 2>/dev/null || true' EXIT

for _ in $(seq 100); do
  HYPRLAND_INSTANCE_SIGNATURE=$(ls "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1)
  [ -n "$HYPRLAND_INSTANCE_SIGNATURE" ] && [ -S "$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock" ] && break
  sleep 0.2
done
export HYPRLAND_INSTANCE_SIGNATURE
hyprctl version | head -1
hyprctl output create headless HEADLESS-1 || true

clients() { hyprctl -j clients | python3 -Ic 'import json,sys; print(len(json.load(sys.stdin)))'; }

hyprctl dispatch 'hl.dsp.exec_cmd("foot")'
hyprctl dispatch 'hl.dsp.exec_cmd("foot")'
for _ in $(seq 100); do [ "$(clients)" = 2 ] && break; sleep 0.2; done
[ "$(clients)" = 2 ] || { echo "foot windows did not appear"; exit 1; }

"$binary" -n

"$binary" -t 5 -c "touch $work/action"
for _ in $(seq 100); do [ -e "$work/action" ] && break; sleep 0.2; done
[ -e "$work/action" ] || { echo "action did not run"; exit 1; }
[ "$(clients)" = 0 ] || { echo "windows left after hyprdown"; hyprctl clients; exit 1; }
kill -0 "$hyprland" || { echo "Hyprland died during an action run"; exit 1; }
grep -q 'all apps exited' "$XDG_STATE_HOME/hyprdown/hyprdown.log" || { echo "windows did not close gracefully"; exit 1; }

"$binary" -t 2
for _ in $(seq 100); do kill -0 "$hyprland" 2>/dev/null || break; sleep 0.2; done
if kill -0 "$hyprland" 2>/dev/null; then echo "Hyprland did not exit"; exit 1; fi
echo "real Hyprland test passed"
