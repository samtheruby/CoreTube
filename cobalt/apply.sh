#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Bring a Cobalt checkout to the pinned commit plus CoreTube's patches.
# Usage: cobalt/apply.sh SRC_DIR
# Prints "applied" or "already applied" (exit 0), or "refusing: <reason>" (exit 3).
# A tree with the first patches already applied gets only the later ones. It
# never resets, cleans or overwrites anything: a tree with other changes is
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

# Trees of the pin plus the first k patches, k = 0..n, one per line of $tmp/trees.
GIT_INDEX_FILE=$tmp/expected g read-tree "$head"
g rev-parse "$head^{tree}" > "$tmp/trees"
for p in "$patches"/*.patch; do
  GIT_INDEX_FILE=$tmp/expected g apply --cached "$p" 2>/dev/null ||
    refuse "$(basename "$p") does not apply to the pinned commit"
  GIT_INDEX_FILE=$tmp/expected g write-tree >> "$tmp/trees"
done
final=$(tail -n 1 "$tmp/trees")
g diff-tree -r --name-only "$(head -n 1 "$tmp/trees")" "$final" > "$tmp/patched"

# Paths where the disk differs from a tree, other than untracked files the
# patches never touch (those are notes).
differences() {
  { g diff-tree -r --diff-filter=A --name-only "$1" "$actual" | grep -xF -f "$tmp/patched"
    g diff-tree -r --diff-filter=a --name-only "$1" "$actual"; } || true
}
notes() {
  g diff-tree -r --diff-filter=A --name-only "$final" "$actual" |
    grep -vxF -f "$tmp/patched" | sed 's/^/note: untracked /' || true
}

# The longest run of patches the disk already has.
done_count=-1 k=0
while read -r tree; do
  [ -n "$(differences "$tree")" ] || done_count=$k
  k=$((k + 1))
done < "$tmp/trees"
total=$((k - 1))
[ "$done_count" -ge 0 ] ||
  refuse "files differ from the pin plus patches: $(differences "$final" | head -20 | tr '\n' ' ')"

if [ "$done_count" -lt "$total" ]; then
  i=0
  for p in "$patches"/*.patch; do
    i=$((i + 1))
    [ "$i" -gt "$done_count" ] || continue
    g apply "$p" || refuse "$(basename "$p") failed to apply"
  done
  notes
  echo "applied"
else
  notes
  echo "already applied"
fi
