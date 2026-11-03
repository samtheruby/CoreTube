#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# tizentube-stop clears every HDMI video mute Cobalt left behind. The kernel's
# vid_mute counts requests: each "1" adds one, each "0" takes one away, and
# reading it gives the count. A fake `cat` plays the kernel's part here.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
mute=$tmp/sys/class/amhdmitx/amhdmitx0/vid_mute
mkdir -p "$(dirname "$mute")" "$tmp/bin"
# A "0" written since the last read takes one request away.
cat > "$tmp/bin/cat" <<EOF
#!/bin/sh
if [ "\$1" = "$mute" ]; then
  n=\$(/bin/cat "$tmp/count")
  if [ "\$(/bin/cat "$mute")" = 0 ] && [ "\$n" -gt 0 ]; then
    n=\$((n - 1)); echo "\$n" > "$tmp/count"; echo "\$n" > "$mute"
  fi
  echo "\$n"
else
  exec /bin/cat "\$@"
fi
EOF
chmod +x "$tmp/bin/cat"
count() { PATH=$tmp/bin:$PATH cat "$mute"; }

# Three mutes outstanding (the sink's and two around a refresh rate switch).
echo 3 > "$tmp/count"; echo 3 > "$mute"
PATH=$tmp/bin:$PATH TIZENTUBE_ROOT=$tmp sh "$repo/addon/bin/tizentube-stop"
[ "$(count)" = 0 ] || fail "vid_mute count is $(count), want 0"

# None outstanding: nothing written.
echo 0 > "$tmp/count"; echo 0 > "$mute"; chmod a-w "$mute"
PATH=$tmp/bin:$PATH TIZENTUBE_ROOT=$tmp sh "$repo/addon/bin/tizentube-stop" 2>"$tmp/err"
[ ! -s "$tmp/err" ] || fail "wrote to an unmuted output: $(/bin/cat "$tmp/err")"
echo "test_video_unmute: ok"
