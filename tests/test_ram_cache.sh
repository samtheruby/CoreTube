#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# tizentube-ram-cache keeps the caches Cobalt can rebuild (HTTP, JavaScript code
# and GPU shader caches) in RAM instead of on the eMMC; cookies, sign-in and
# TizenTube's settings stay where they are.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
home=$tmp/home ram=$tmp/run/tizentube-cache
youtube=aHR0cHM6Ly93d3cueW91dHViZS5jb20vdHY=  # https://www.youtube.com/tv
other=aHR0cDovLzEyNy4wLjAuMTo4NzY1Lw==        # another site Cobalt opened
run() { sh "$repo/addon/bin/tizentube-ram-cache" "$home" "$ram"; }

# A home from an earlier version: caches and data on the eMMC.
for site in $youtube $other; do
  mkdir -p "$home/.cache/cobalt/$site/Cache/Cache_Data" "$home/.cache/cobalt/$site/Code Cache/js" \
    "$home/.cache/cobalt/$site/WebStorage" "$home/.cobalt_files/$site/GPUCache" \
    "$home/.cobalt_files/$site/Local Storage/leveldb"
  echo old > "$home/.cache/cobalt/$site/Cache/Cache_Data/entry"
  echo quota > "$home/.cache/cobalt/$site/WebStorage/QuotaManager"
  echo cookie > "$home/.cobalt_files/$site/Cookies"
  echo settings > "$home/.cobalt_files/$site/Local Storage/leveldb/000001.log"
done
run

for site in $youtube $other; do
  for d in ".cache/cobalt/$site/Cache" ".cache/cobalt/$site/Code Cache" \
      ".cobalt_files/$site/GPUCache" ".cobalt_files/$site/DawnGraphiteCache" \
      ".cobalt_files/$site/DawnWebGPUCache"; do
    [ -L "$home/$d" ] || fail "$d is not a link into RAM"
    [ "$(readlink "$home/$d")" = "$ram/$d" ] || fail "$d points at $(readlink "$home/$d")"
    [ -d "$ram/$d" ] || fail "$ram/$d was not created"
  done
  [ "$(cat "$home/.cobalt_files/$site/Cookies")" = cookie ] || fail "cookies changed"
  [ "$(cat "$home/.cobalt_files/$site/Local Storage/leveldb/000001.log")" = settings ] ||
    fail "TizenTube's settings changed"
  [ "$(cat "$home/.cache/cobalt/$site/WebStorage/QuotaManager")" = quota ] || fail "quota data changed"
done
[ ! -e "$ram/.cache/cobalt/$youtube/Cache/Cache_Data/entry" ] || fail "old cache copied into RAM"

# The next start (same boot): the caches in RAM are kept.
echo cached > "$ram/.cache/cobalt/$youtube/Cache/entry"
run
[ "$(cat "$home/.cache/cobalt/$youtube/Cache/entry")" = cached ] || fail "RAM cache lost on the next start"

# After a reboot RAM is empty: the links get their folders back.
rm -rf "$tmp/run"
run
[ -d "$home/.cache/cobalt/$youtube/Cache/" ] || fail "link left dangling after a reboot"

# A fresh home: YouTube's caches go to RAM from the first start.
rm -rf "$home" "$tmp/run"
run
[ -L "$home/.cache/cobalt/$youtube/Code Cache" ] || fail "fresh home: code cache not in RAM"
[ -L "$home/.cobalt_files/$youtube/GPUCache" ] || fail "fresh home: GPU cache not in RAM"
echo "test_ram_cache: ok"
