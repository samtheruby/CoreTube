#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Bring a Cobalt checkout to the pinned commit plus CoreTube's patches.
# Usage: cobalt/apply.sh SRC_DIR
# Prints "applied" or "already applied" (exit 0), or "refusing: <reason>" (exit 3).
# It never resets, cleans or overwrites anything: a tree with other changes is
# refused. Untracked files the patches do not touch are listed as notes.
# COBALT_COMMIT and COBALT_PATCHES override versions.env and cobalt/patches.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: cobalt/apply.sh SRC_DIR}
pin=${COBALT_COMMIT:-$(sed -n 's/^COBALT_COMMIT=//p' "$here/../versions.env")}
patches=${COBALT_PATCHES:-$here/patches}

refuse() { echo "refusing: $*"; exit 3; }
g() { git -C "$src" "$@"; }

head=$(g rev-parse HEAD) || refuse "$src is not a git checkout"
[ "$head" = "$pin" ] || refuse "HEAD $head is not the pinned $pin"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
index=$(g rev-parse --absolute-git-dir)/index

# Tree of what is on disk now (tracked and untracked, minus ignored files).
# A copy of the real index keeps git's stat cache, so only changed files are read.
cp "$index" "$tmp/actual"
GIT_INDEX_FILE=$tmp/actual g add -A
actual=$(GIT_INDEX_FILE=$tmp/actual g write-tree)

# Tree the pin plus our patches should give.
GIT_INDEX_FILE=$tmp/expected g read-tree "$head"
for p in "$patches"/*.patch; do
  GIT_INDEX_FILE=$tmp/expected g apply --cached "$p" 2>/dev/null ||
    refuse "$(basename "$p") does not apply to the pinned commit"
done
expected=$(GIT_INDEX_FILE=$tmp/expected g write-tree)
base=$(g rev-parse "$head^{tree}")

g diff-tree -r --name-only "$base" "$expected" > "$tmp/patched"

# Files on disk that neither the pin nor the patches have.
notes() {
  g diff-tree -r --diff-filter=A --name-only "$1" "$actual" |
    grep -vxF -f "$tmp/patched" | sed 's/^/note: untracked /' || true
}
# Paths where the disk differs from a tree, other than those notes.
differences() {
  { g diff-tree -r --diff-filter=A --name-only "$1" "$actual" | grep -xF -f "$tmp/patched"
    g diff-tree -r --diff-filter=a --name-only "$1" "$actual"; } || true
}

if [ -z "$(differences "$expected")" ]; then
  notes "$expected"
  echo "already applied"
  exit 0
fi
[ -z "$(differences "$base")" ] ||
  refuse "files differ from the pin plus patches: $(differences "$expected" | head -20 | tr '\n' ' ')"

for p in "$patches"/*.patch; do
  g apply "$p" || refuse "$(basename "$p") failed to apply"
done
notes "$base"
echo "applied"
