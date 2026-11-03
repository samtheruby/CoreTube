#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# cobalt/apply.sh on a tiny scratch repo: applies once, detects an applied tree,
# refuses a wrong pin or foreign edits, and only notes unrelated untracked files.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

g() { git -C "$tmp/src" -c user.name=t -c user.email=t@t "$@"; }
mkdir "$tmp/src" "$tmp/patches"
git init -q "$tmp/src"
printf 'one\n' > "$tmp/src/a.txt"
printf 'two\n' > "$tmp/src/b.txt"
g add -A && g commit -q -m base
pin=$(g rev-parse HEAD)
# Patch 1 edits a tracked file, patch 2 adds a new one.
printf 'one changed\n' > "$tmp/src/a.txt"
g diff > "$tmp/patches/0001-edit.patch"
g checkout -q a.txt
printf 'new\n' > "$tmp/src/c.txt"
g add -N c.txt && g diff > "$tmp/patches/0002-add.patch"
g rm -q --cached c.txt && rm "$tmp/src/c.txt"

run() { COBALT_COMMIT=$1 COBALT_PATCHES="$tmp/patches" sh "$repo/cobalt/apply.sh" "$tmp/src" 2>&1; }

out=$(run "$pin") || fail "clean tree: exit $? ($out)"
[ "$out" = applied ] || fail "clean tree printed: $out"
grep -qx 'one changed' "$tmp/src/a.txt" || fail "patch 1 not applied"
[ -f "$tmp/src/c.txt" ] || fail "patch 2 not applied"

out=$(run "$pin") || fail "second run: exit $? ($out)"
[ "$out" = "already applied" ] || fail "second run printed: $out"

printf 'stray\n' > "$tmp/src/notes.txt"
out=$(run "$pin") || fail "untracked file: exit $? ($out)"
echo "$out" | grep -qx 'note: untracked notes.txt' || fail "untracked file not noted: $out"
echo "$out" | grep -qx 'already applied' || fail "untracked file changed the verdict: $out"
rm "$tmp/src/notes.txt"

printf 'local edit\n' >> "$tmp/src/b.txt"
set +e; out=$(run "$pin"); rc=$?; set -e
[ "$rc" -eq 3 ] || fail "foreign edit: exit $rc ($out)"
echo "$out" | grep -q '^refusing: ' || fail "foreign edit printed: $out"
grep -qx 'local edit' "$tmp/src/b.txt" || fail "foreign edit was touched"
g checkout -q b.txt

set +e; out=$(run 0000000000000000000000000000000000000000); rc=$?; set -e
[ "$rc" -eq 3 ] || fail "wrong pin: exit $rc ($out)"
echo "$out" | grep -q '^refusing: HEAD' || fail "wrong pin printed: $out"
echo "test_cobalt_apply: ok"
