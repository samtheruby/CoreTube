#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Test the page script Cobalt patch 0009 adds (coretube_sabr.js), taken from the patch.
set -eu
cd "$(dirname "$0")/.."
command -v node >/dev/null || { echo "test_sabr_script: node not found" >&2; exit 1; }
patch=$(ls cobalt/patches/0009-*.patch)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The lines of the new file: the '+' lines of its hunk, without the '+'.
awk '/^diff --git / { on = 0 }
     /^\+\+\+ b\/.*\/coretube_sabr\.js$/ { on = 1; next }
     on && /^@@/ { next }
     on && /^\+/ { print substr($0, 2) }' "$patch" > "$tmp/coretube_sabr.js"
[ -s "$tmp/coretube_sabr.js" ] || { echo "test_sabr_script: no coretube_sabr.js in $patch" >&2; exit 1; }
node tests/sabr_script_test.js "$tmp/coretube_sabr.js"
