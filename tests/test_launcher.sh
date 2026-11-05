#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# The launcher ends the process as soon as Cobalt's StarboardMain returns, with
# its exit code and its output flushed, but without the global destructors:
# Cobalt leaves its renderer thread running, and those destructors freed
# objects under it (a crash and a core dump at every exit).
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
cc=${CC:-cc}
command -v "$cc" >/dev/null || { echo "test_launcher: $cc not found" >&2; exit 1; }

# A stand-in for Cobalt: buffered output, a global destructor, exit code 3.
cat > "$tmp/fake_cobalt.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
static void destructor(void) { puts("destructor ran"); }
int StarboardMain(int argc, char** argv) {
  (void)argc; (void)argv;
  atexit(destructor);
  printf("cobalt stopped\n");
  return 3;
}
EOF
"$cc" -Wall -Werror -o "$tmp/launcher" "$repo/addon/launcher.c" "$tmp/fake_cobalt.c"
status=0
"$tmp/launcher" > "$tmp/out" || status=$?
[ "$status" = 3 ] || fail "exit code $status, want 3"
grep -qx "cobalt stopped" "$tmp/out" || fail "Cobalt's output was lost: $(cat "$tmp/out")"
! grep -q "destructor ran" "$tmp/out" || fail "global destructors ran"
echo "test_launcher: ok"
