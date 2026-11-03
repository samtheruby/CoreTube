#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# build-coretube.sh option handling and step plan (--dry-run touches nothing).
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
mkdir "$tmp/build" "$tmp/cobalt"

# expect CODE TEXT ARGS...: exit code CODE and TEXT somewhere in the output.
expect() {
  code=$1 text=$2
  shift 2
  set +e
  out=$(bash "$repo/build-coretube.sh" "$@" 2>&1)
  rc=$?
  set -e
  [ "$rc" -eq "$code" ] || fail "$* -> exit $rc, want $code: $out"
  echo "$out" | grep -qF -- "$text" || fail "$* -> no '$text' in: $out"
}

expect 2 "usage:"
expect 2 "unknown --arch: mips" --arch mips --build-dir "$tmp/build" --cobalt-dir "$tmp/cobalt"
expect 2 "--build-dir is required" --arch arm --cobalt-dir "$tmp/cobalt"
expect 2 "--cobalt-dir is required" --arch arm --build-dir "$tmp/build"
expect 2 "--build-dir $tmp/nope does not exist" --arch arm --build-dir "$tmp/nope" --cobalt-dir "$tmp/cobalt"
expect 2 "unknown step: paint" --arch arm --build-dir "$tmp/build" --cobalt-dir "$tmp/cobalt" --only paint

plan() {
  bash "$repo/build-coretube.sh" --dry-run --arch "$1" --build-dir "$tmp/build" --cobalt-dir "$tmp/cobalt" --work-dir "$tmp/work"
}
out=$(plan arm)
[ "$(echo "$out" | sed -n 's/^step \([a-z]*\):.*/\1/p' | tr '\n' ' ')" = "sources packages cobalt helper zip " ] ||
  fail "arm steps: $out"
for want in coretube-ng build.CoreELEC-Amlogic-ng.arm-21 evergreen-arm-hardfp-coreelec out/arm/browser.tizentube-; do
  echo "$out" | grep -qF "$want" || fail "arm plan lacks $want: $out"
done
out=$(plan aarch64)
for want in coretube-no build.CoreELEC-Amlogic-no.aarch64-22 evergreen-arm64-coreelec out/aarch64/browser.tizentube-; do
  echo "$out" | grep -qF "$want" || fail "aarch64 plan lacks $want: $out"
done
[ ! -e "$tmp/work" ] || fail "--dry-run created the work folder"

# Default parallel jobs: 75% of the CPUs, at least one.
want=$(( $(nproc) * 3 / 4 )); [ "$want" -ge 1 ] || want=1
echo "$out" | grep -qx "jobs: $want" || fail "default jobs not $want: $out"
out=$(bash "$repo/build-coretube.sh" --dry-run --arch arm --build-dir "$tmp/build" --cobalt-dir "$tmp/cobalt" -j 3)
echo "$out" | grep -qx "jobs: 3" || fail "-j 3 not used: $out"
expect 2 "-j needs a whole number" --arch arm --build-dir "$tmp/build" --cobalt-dir "$tmp/cobalt" -j many
echo "test_build_script: ok"
