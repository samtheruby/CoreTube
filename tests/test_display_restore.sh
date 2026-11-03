#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# tizentube-stop puts the display mode back to what tizentube-start saved, so a
# crash after an auto frame rate switch does not leave Kodi at 23.976 Hz.
# TIZENTUBE_ROOT points both scripts' /sys and /run at a fake tree.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
mode=$tmp/sys/class/display/mode
frac=$tmp/sys/class/amhdmitx/amhdmitx0/frac_rate_policy
mkdir -p "$(dirname "$mode")" "$(dirname "$frac")" "$tmp/run"
stop() { TIZENTUBE_ROOT=$tmp sh "$repo/addon/bin/tizentube-stop"; }

# Switched to 23.976 Hz, then Cobalt ended: back to 60 Hz.
echo 1080p24hz > "$mode"; echo 1 > "$frac"
echo "1080p60hz 0" > "$tmp/run/tizentube-display-state"
stop
[ "$(cat "$mode")" = 1080p60hz ] || fail "mode is $(cat "$mode"), want 1080p60hz"
[ "$(cat "$frac")" = 0 ] || fail "frac_rate_policy is $(cat "$frac"), want 0"
[ ! -e "$tmp/run/tizentube-display-state" ] || fail "saved state not removed"

# Nothing saved (no start, or already restored): the display is left alone.
echo 2160p24hz > "$mode"
stop 2>"$tmp/err"
[ "$(cat "$mode")" = 2160p24hz ] || fail "mode changed without a saved state"
[ ! -s "$tmp/err" ] || fail "complained about a missing state: $(cat "$tmp/err")"

# Unchanged display: no writes (a mode write makes the TV blink).
echo "1080p60hz 0" > "$tmp/run/tizentube-display-state"
echo 1080p60hz > "$mode"; echo 0 > "$frac"
chmod a-w "$mode" "$frac"
stop 2>"$tmp/err" || fail "stop failed: $(cat "$tmp/err")"
[ ! -s "$tmp/err" ] || fail "wrote to an unchanged display: $(cat "$tmp/err")"
echo "test_display_restore: ok"
