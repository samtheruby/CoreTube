#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# --docker with a root-owned build folder (e.g. made by root on a NAS): the packages
# step creates CoreELEC's folder for the image's build user (1000) and builds as it,
# but still refuses a CoreELEC folder that already belongs to root.
# A fake docker on PATH plays the containers.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
mkdir "$tmp/bin" "$tmp/build" "$tmp/cobalt"

# The fake answers "stat" with $tmp/owner, creates the folder on "mkdir", and logs
# which user each build-coretube.sh run would use.
cat > "$tmp/bin/docker" <<EOF
#!/bin/sh
case "\$*" in
  "image inspect"*) exit 0 ;;
  *mkdir*) echo 1000:1000 > "$tmp/owner"; echo prepared >> "$tmp/log" ;;
  *stat*) cat "$tmp/owner" ;;
  *build-coretube.sh*) echo "build as \$(echo "\$*" | sed -n 's/.*--user \([^ ]*\).*/\1/p')" >> "$tmp/log" ;;
esac
EOF
chmod +x "$tmp/bin/docker"
run() {
  : > "$tmp/log"
  PATH="$tmp/bin:$PATH" bash "$repo/build-coretube.sh" --docker --arch arm \
    --build-dir "$tmp/build" --cobalt-dir "$tmp/cobalt" --work-dir "$tmp/work" --only packages 2>&1
}

echo 0:0 > "$tmp/owner"
out=$(run) || fail "root-owned new build folder: $out"
grep -qx prepared "$tmp/log" || fail "CoreELEC folder not prepared: $(cat "$tmp/log")"
grep -qx "build as 1000:1000" "$tmp/log" || fail "not built as 1000:1000: $(cat "$tmp/log")"

# A CoreELEC folder that already belongs to root stays refused (stat keeps saying 0:0).
cat > "$tmp/bin/docker" <<EOF
#!/bin/sh
case "\$*" in
  "image inspect"*) exit 0 ;;
  *stat*) echo 0:0 ;;
  *build-coretube.sh*) echo built >> "$tmp/log" ;;
esac
EOF
set +e; out=$(run); rc=$?; set -e
[ "$rc" -ne 0 ] || fail "root-owned CoreELEC folder was accepted"
echo "$out" | grep -q "refuses to build as root" || fail "no clear refusal: $out"
! grep -q built "$tmp/log" || fail "built despite the refusal"
echo "test_docker_owner: ok"
