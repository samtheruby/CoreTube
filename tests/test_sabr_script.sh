#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Test the page script the Cobalt patches add (coretube_sabr.js): 0009 adds it,
# later patches change it; each patch's part for that file applies in order.
set -eu
cd "$(dirname "$0")/.."
command -v node >/dev/null || { echo "test_sabr_script: node not found" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
root=$(pwd)
for p in cobalt/patches/*.patch; do
  grep -q '^+++ b/.*/coretube_sabr\.js$' "$p" || continue
  (cd "$tmp" && git apply --include='*/coretube_sabr.js' "$root/$p")
done
script=$(find "$tmp" -name coretube_sabr.js)
[ -n "$script" ] || { echo "test_sabr_script: no patch adds coretube_sabr.js" >&2; exit 1; }
node tests/sabr_script_test.js "$script"
