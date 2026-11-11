#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Fail if a tracked file holds personal or secret data: home-network addresses,
# paths of one build machine, private keys, DRM state (a provisioned profile), or
# the Widevine libraries (they stay out of git; see README.md).
# Usage: tests/check_repo.sh   (from anywhere inside the repo)
set -eu
cd "$(git rev-parse --show-toplevel)"
self=tests/check_repo.sh
found=0

# File contents.
git ls-files -z | xargs -0 grep -I -n -E \
  -e '192\.168\.[0-9]' -e '/mnt/(cache|user)' -e '/workspace/' -e '/home/claude' \
  -e 'BEGIN [A-Z ]*PRIVATE KEY' -- 2>/dev/null |
  grep -v "^$self:" && found=1

# File names.
git ls-files | grep -E '(^|/)android-drm/|\.pem$|\.key$|(^|/)id_(rsa|ecdsa|ed25519)' && found=1
git ls-files | grep -E '^drm/runtime/|coretube-widevine\.zip$|runtime-cube-armv7\.tar\.gz$|(^|/)libwvhidl\.so$' && found=1

if [ "$found" -ne 0 ]; then
  echo "check_repo: personal or secret data found (see above)" >&2
  exit 1
fi
echo "check_repo: clean"
