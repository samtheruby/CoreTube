#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Fetch the pinned tooling the Android helper builds with into TOOLING_DIR:
# the Android NDK (SHA-256 checked) and the AOSP Android 9 headers it needs.
# Usage: drm/tools/fetch-tooling.sh TOOLING_DIR
# Re-running skips what is already there at the pinned version.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dir=${1:?usage: drm/tools/fetch-tooling.sh TOOLING_DIR}
. "$here/../../versions.env"
mkdir -p "$dir"
dir=$(cd "$dir" && pwd)

ndk_zip=$dir/android-ndk-r28-linux.zip
if [ ! -x "$dir/android-ndk-r28/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]; then
  [ -f "$ndk_zip" ] || curl -fsSL -o "$ndk_zip" "$NDK_URL"
  echo "$NDK_SHA256  $ndk_zip" | sha256sum -c --quiet - ||
    { echo "fetch-tooling: NDK checksum mismatch: $ndk_zip" >&2; exit 1; }
  (cd "$dir" && unzip -q "$ndk_zip")
fi

# fetch NAME URL COMMIT PATH...: a shallow, partial checkout of only PATHs.
fetch() {
  name=$1 url=$2 commit=$3
  shift 3
  repo=$dir/$name
  if [ -d "$repo/.git" ] && [ "$(git -C "$repo" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
    return
  fi
  rm -rf "$repo"
  git init -q "$repo"
  git -C "$repo" remote add origin "$url"
  git -C "$repo" sparse-checkout set --no-cone "$@"
  git -C "$repo" fetch -q --depth 1 --filter=blob:none origin "$commit"
  git -C "$repo" checkout -q FETCH_HEAD
}
fetch core9 "$AOSP_CORE_URL" "$AOSP_CORE_COMMIT" /libutils/include/ /libcutils/include/ /liblog/include/ /liblog/include_vndk/
fetch libcxx9 "$AOSP_LIBCXX_URL" "$AOSP_LIBCXX_COMMIT" /include/
fetch libhidl9 "$AOSP_LIBHIDL_URL" "$AOSP_LIBHIDL_COMMIT" /base/include/ /transport/include/
fetch vndk30 "$AOSP_VNDK30_URL" "$AOSP_VNDK30_COMMIT" /arm/include/
echo "fetch-tooling: ready in $dir"
