#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# CoreTube's CoreELEC packages carry their own names, so they can share a build
# folder with the WebKit add-on's packages without overwriting them.
set -eu
cd "$(dirname "$0")/../coreelec/packages"
fail() { echo "FAIL: $*" >&2; exit 1; }

want="coretube-amlvideosink coretube-gst-libav coretube-gst-plugins-bad coretube-gst-plugins-base coretube-gst-plugins-good coretube-gstreamer"
have=$(ls -d */ 2>/dev/null | tr -d / | sort | tr '\n' ' ' | sed 's/ $//')
[ "$have" = "$want" ] || fail "packages are '$have', want '$want'"

! grep -rn -i 'webkit' . || fail "a package still names WebKit"
for p in $want; do
  name=$(sed -n 's/^PKG_NAME="\(.*\)"/\1/p' "$p/package.mk")
  [ "$name" = "$p" ] || fail "$p/package.mk has PKG_NAME=$name"
  for dep in $(sed -n 's/^PKG_DEPENDS_TARGET="\(.*\)"/\1/p' "$p/package.mk"); do
    case $dep in coretube-*) [ -d "$dep" ] || fail "$p depends on missing $dep" ;; esac
  done
  # Everything installs under a private prefix, so the image's own GStreamer in
  # the shared CoreELEC sysroot stays untouched, and finds its siblings there.
  grep -q '^PKG_MESON_OPTS_TARGET="${CORETUBE_MESON_OPTS}' "$p/package.mk" ||
    fail "$p does not start PKG_MESON_OPTS_TARGET with CORETUBE_MESON_OPTS"
  grep -qx '. $(get_pkg_directory coretube-gstreamer)/coretube.mk' "$p/package.mk" ||
    fail "$p does not include coretube.mk"
done
echo "test_package_names: ok"
