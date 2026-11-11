#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Build the Android Widevine helper (32-bit ARM, Android 9 Bionic) that libocdm
# talks to. It links the unmodified libraries from the Cube's Widevine zip.
# Usage: TOOLING_DIR=... COBALT_SRC=... drm/helper/build.sh OUT_FILE
#   TOOLING_DIR: from drm/tools/fetch-tooling.sh
#   COBALT_SRC:  patched Cobalt src (for the shared android_protocol.h)
#   RUNTIME:     coretube-widevine.zip (default: drm/runtime/, not in git)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: drm/helper/build.sh OUT_FILE}
tooling=${TOOLING_DIR:?set TOOLING_DIR (see drm/tools/fetch-tooling.sh)}
protocol=${COBALT_SRC:?set COBALT_SRC to the patched Cobalt src}/starboard/contrib/rdk/src/third_party/starboard/rdk/coreelec/ocdm_widevine
runtime=${RUNTIME:-$here/../runtime/coretube-widevine.zip}
[ -f "$protocol/android_protocol.h" ] || { echo "build.sh: no android_protocol.h in $protocol (Cobalt not patched?)" >&2; exit 1; }
[ -f "$runtime" ] || { echo "build.sh: no Widevine zip at $runtime (see README.md, Widevine)" >&2; exit 1; }
python3 -I "$here/../tools/runtime_bundle.py" check "$runtime"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Link against the Cube's own libraries; libc and libdl come from the NDK.
mkdir "$tmp/stock"
python3 -I -m zipfile -e "$runtime" "$tmp"
for lib in vendor/lib/libwvhidl.so system/lib/liblog.so \
  system/lib/vndk-28/android.hardware.drm@1.0.so system/lib/vndk-28/android.hardware.drm@1.1.so \
  system/lib/vndk-sp-28/{android.hidl.memory@1.0.so,libbase.so,libc++.so,libcutils.so,libhidlbase.so,libhidltransport.so,libhwbinder.so,libutils.so}; do
  cp "$tmp/$lib" "$tmp/stock/"
done

compiler=$tooling/android-ndk-r28/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi28-clang++
flags=(-std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti
       -nostdinc++ -nostdlib++ -include "$here/hidl_generated_metadata.h" -I "$protocol")
for directory in \
  "$tooling/libcxx9/include" \
  "$tooling/libhidl9/base/include" \
  "$tooling/libhidl9/transport/include" \
  "$tooling/core9/libutils/include" \
  "$tooling/core9/libcutils/include" \
  "$tooling/core9/liblog/include_vndk" \
  "$tooling/core9/liblog/include" \
  "$tooling/vndk30/arm/include/system/core/libutils/include"; do
  flags+=(-isystem "$directory")
done
while IFS= read -r -d '' directory; do
  flags+=(-isystem "$directory")
done < <(find "$tooling/vndk30/arm/include/generated-headers" -type d -name gen -print0)
"$compiler" "${flags[@]}" "$here/android_helper.cc" "$here/mapped_memory.cc" -L"$tmp/stock" \
  -Wl,-rpath-link,"$tmp/stock" \
  -l:android.hardware.drm@1.1.so -l:android.hardware.drm@1.0.so \
  -Wl,--export-dynamic -l:android.hidl.memory@1.0.so \
  -lhidlbase -lhidltransport -lhwbinder -lutils -lcutils -lc++ -ldl -o "$out"
