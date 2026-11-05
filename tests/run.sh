#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Run every host test that needs no build folder. Exit non-zero on any failure.
set -eu
cd "$(dirname "$0")/.."

sh tests/check_repo.sh
for t in tests/test_*.sh; do
  [ -e "$t" ] || continue
  echo "== $t"
  sh "$t"
done
dirs=""
for d in tests addon/tests drm/tools/tests; do
  [ -d "$d" ] && dirs="$dirs $d"
done
if [ -n "$dirs" ]; then
  python3 -m pytest -q $dirs
fi
echo "tests/run.sh: all passed"
