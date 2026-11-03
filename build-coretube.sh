#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Build the TizenTube Kodi add-on zip for CoreELEC. See README.md.
set -euo pipefail

USAGE="usage: build-coretube.sh --arch arm|aarch64 --build-dir DIR --cobalt-dir DIR [options]
       build-coretube.sh test [--docker] [--arch A --build-dir DIR --cobalt-dir DIR]

  --arch arm|aarch64  32-bit CoreELEC 21 (Amlogic-ng) or 64-bit CoreELEC 22 (Amlogic-no)
  --build-dir DIR     CoreELEC build folder (BUILD_DIR); reused, filled when empty
  --cobalt-dir DIR    Cobalt gclient folder (holds src/ and depot_tools/); reused
  --work-dir DIR      fetched sources and scratch (default: work/ in this repo)
  --docker            run every step in the matching image (docker/*.Dockerfile)
  --only STEP         run one step: sources, packages, cobalt, helper or zip
  --allow-rebuild     let the packages step rebuild CoreELEC packages that the
                      build folder has from other sources (they get replaced)
  --dry-run           print the plan and stop
  -j N                parallel jobs for the Cobalt build (default: 75% of the CPUs)

Set DOCKER_PATH_MAP=/inside=/on-host[:...] when this script runs in a container
that talks to the host's Docker, so mounted paths are translated."

repo=$(cd "$(dirname "$0")" && pwd)
. "$repo/versions.env"
all_steps="sources packages cobalt helper zip"
packages="coretube-amlvideosink coretube-gst-plugins-good coretube-gst-plugins-bad coretube-gst-libav"

usage_error() { echo "build-coretube.sh: $*" >&2; echo "$USAGE" >&2; exit 2; }
die() { echo "build-coretube.sh: $*" >&2; exit 1; }

mode="build" arch="" build_dir="" cobalt_dir="" work_dir="$repo/work"
docker=0 dry_run=0 only="" allow_rebuild=0
# Leave a quarter of the machine free for everything else.
jobs=$(( $(nproc) * 3 / 4 ))
[ "$jobs" -ge 1 ] || jobs=1
[ $# -gt 0 ] || usage_error "no arguments"
if [ "$1" = test ]; then mode="test"; shift; fi
while [ $# -gt 0 ]; do
  case $1 in
    --arch | --build-dir | --cobalt-dir | --work-dir | --only | -j)
      [ $# -ge 2 ] || usage_error "$1 needs a value"
      case $1 in
        --arch) arch=$2 ;; --build-dir) build_dir=$2 ;; --cobalt-dir) cobalt_dir=$2 ;;
        --work-dir) work_dir=$2 ;; --only) only=$2 ;; -j) jobs=$2 ;;
      esac
      shift 2 ;;
    --docker) docker=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --allow-rebuild) allow_rebuild=1; shift ;;
    -h | --help) echo "$USAGE"; exit 0 ;;
    *) usage_error "unknown option: $1" ;;
  esac
done

# test mode may run without --arch.
image="" ce_url="" ce_commit="" ce_device="" ce_subdir="" triple="" cc_flags=""
platform="" stock="" sysroot_arch="" legacy=""
case $arch in
  arm)
    image=coretube-ng ce_url=$COREELEC_NG_URL ce_commit=$COREELEC_NG_COMMIT
    ce_device=Amlogic-ng ce_subdir=build.CoreELEC-Amlogic-ng.arm-21
    triple=armv8a-libreelec-linux-gnueabihf cc_flags=""
    platform=evergreen-arm-hardfp-coreelec stock=evergreen-arm-hardfp sysroot_arch=arm
    # Engine of the earlier RDK-SDK builds; same ARM hard-float ABI, reused if present.
    legacy=evergreen-arm-hardfp-rdk ;;
  aarch64)
    image=coretube-no ce_url=$COREELEC_NO_URL ce_commit=$COREELEC_NO_COMMIT
    ce_device=Amlogic-no ce_subdir=build.CoreELEC-Amlogic-no.aarch64-22
    triple=aarch64-libreelec-linux-gnu cc_flags="-fuse-ld=gold"
    platform=evergreen-arm64-coreelec stock=evergreen-arm64 sysroot_arch=arm64 legacy="" ;;
  "") [ "$mode" = test ] || usage_error "--arch is required" ;;
  *) usage_error "unknown --arch: $arch (use arm or aarch64)" ;;
esac
if [ "$mode" = build ]; then
  [ -n "$build_dir" ] || usage_error "--build-dir is required"
  [ -n "$cobalt_dir" ] || usage_error "--cobalt-dir is required"
fi
case " $all_steps " in *" ${only:-sources} "*) ;; *) usage_error "unknown step: $only" ;; esac
[[ $jobs =~ ^[1-9][0-9]*$ ]] || usage_error "-j needs a whole number: $jobs"
# With --docker the folders may be host paths this shell cannot see.
for option in build_dir cobalt_dir; do
  dir=${!option}
  [ -z "$dir" ] || [ "$docker" = 1 ] || [ -d "$dir" ] ||
    usage_error "--${option/_/-} $dir does not exist (create it for a fresh build)"
done
work_dir=$(realpath -m "$work_dir")
[ -z "$build_dir" ] || build_dir=$(realpath -m "$build_dir")
[ -z "$cobalt_dir" ] || cobalt_dir=$(realpath -m "$cobalt_dir")

version=$(python3 -I -c 'import sys, xml.etree.ElementTree as E; print(E.parse(sys.argv[1]).getroot().get("version"))' "$repo/addon/addon.xml")
zip_file=$repo/out/$arch/browser.tizentube-$version.zip
ce_build=$build_dir/$ce_subdir
# The CoreELEC checkout lives in CoreELEC's own build folder: the packages step,
# which runs as that folder's owner, is the only one that writes it.
ce=$ce_build/coretube-coreelec
src=$cobalt_dir/src
out=$src/out/${platform}_qa

# --- docker ------------------------------------------------------------------

map_path() {
  local path=$1 rule from
  local -a rules
  IFS=: read -ra rules <<< "${DOCKER_PATH_MAP:-}"
  for rule in "${rules[@]}"; do
    from=${rule%%=*}
    case $path in "$from" | "$from"/*) echo "${rule#*=}${path#"$from"}"; return ;; esac
  done
  echo "$path"
}

ensure_image() {
  docker image inspect "$1" >/dev/null 2>&1 ||
    docker build -t "$1" -f "$repo/docker/${1#coretube-}.Dockerfile" "$repo/docker"
}

# owner IMAGE DIR: uid:gid of a host folder, read inside a container.
# step_folder STEP: "ROOT SUB" - the step runs as the owner of ROOT/SUB, or of ROOT
# while SUB does not exist yet, and writes only there. A build folder can be a
# share owned by someone else that holds CoreELEC's own folder (build.CoreELEC-*).
step_folder() {
  case $1 in
    packages) echo "$build_dir $ce_subdir" ;; cobalt) echo "$cobalt_dir ." ;;
    zip) echo "$repo out" ;; *) echo "$work_dir ." ;;
  esac
}

# owner_folder STEP: that folder, as this shell sees it (for --dry-run).
owner_folder() {
  local root sub
  read -r root sub <<< "$(step_folder "$1")"
  if [ "$sub" != . ] && [ -d "$root/$sub" ]; then echo "$root/$sub"; else echo "$root"; fi
}

# owner IMAGE STEP: uid:gid of that folder, read inside a container (it may be a
# host path this shell cannot see).
owner() {
  local root sub
  read -r root sub <<< "$(step_folder "$2")"
  docker run --rm -v "$(map_path "$root")":/d "$1" sh -c 'stat -c %u:%g "/d/$1" 2>/dev/null || stat -c %u:%g /d' _ "$sub"
}

in_docker() {
  local step=$1 user
  user=$(owner "$image" "$step")
  if [ "$step" = packages ] && [ "${user%%:*}" = 0 ]; then
    # A root-owned build folder (made by root on a NAS, say): give CoreELEC's own
    # folder, if it does not exist yet, to the images' build user (uid 1000).
    docker run --rm --user 0 -v "$(map_path "$build_dir")":/d "$image" \
      sh -c '[ -e "/d/$1" ] || { mkdir -p "/d/$1" && chown 1000:1000 "/d/$1"; }' _ "$ce_subdir"
    user=$(owner "$image" "$step")
  fi
  [ "$step" != packages ] || [ "${user%%:*}" != 0 ] ||
    die "$ce_build (or $build_dir) belongs to root; CoreELEC refuses to build as root (chown it to your user)"
  local -a extra=()
  [ "$allow_rebuild" = 0 ] || extra+=(--allow-rebuild)
  echo "== $step (docker $image, user $user)"
  docker run --rm --user "$user" -e HOME=/tmp/home \
    -v "$(map_path "$repo")":/coretube -v "$(map_path "$work_dir")":/work \
    -v "$(map_path "$build_dir")":/build -v "$(map_path "$cobalt_dir")":/cobalt \
    "$image" bash /coretube/build-coretube.sh --arch "$arch" --build-dir /build \
    --cobalt-dir /cobalt --work-dir /work --only "$step" -j "$jobs" "${extra[@]}"
}

# --- steps -------------------------------------------------------------------

# fetch_git DIR URL COMMIT: a shallow checkout of COMMIT in a folder we own.
fetch_git() {
  if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" = "$3" ]; then return; fi
  [ -d "$1/.git" ] || { git init -q "$1"; git -C "$1" remote add origin "$2"; }
  git -C "$1" fetch -q --depth 1 origin "$3"
  git -C "$1" checkout -q -f FETCH_HEAD
}

step_sources() {
  sh "$repo/drm/tools/fetch-tooling.sh" "$work_dir/tooling"
}

ce_env() { export BUILD_DIR=$build_dir PROJECT=Amlogic-ce DEVICE=$ce_device ARCH=$arch; }

# CoreELEC packages, other than ours, that a build would rebuild because the build
# folder has them built from other sources. Never-built ones are not listed.
stale_packages() {
  # shellcheck disable=SC2086  # one argument per package
  (cd "$ce" && scripts/pkgjson | scripts/genbuildplan.py --hide-wants --build $packages) |
  while read -r action package; do
    [ "$action" = build ] || continue
    case $package in coretube-*) continue ;; esac
    (cd "$ce" && bash -c '. config/options "$1" >/dev/null 2>&1
      s=$STAMPS/$PKG_NAME/build_$2
      [ -f "$s" ] || exit 0
      [ "$(sed -n "s/^STAMP_PKG_DEEPHASH=\"\(.*\)\"/\1/p" "$s")" = "$(calculate_stamp)" ] || echo "$1:$2"' \
      _ "${package%%:*}" "$( [ "${package#*:}" = "$package" ] && echo target || echo "${package#*:}")")
  done
}

step_packages() {
  fetch_git "$ce" "$ce_url" "$ce_commit"
  rm -rf "$ce/packages/addons/addon-depends/coretube-depends"
  cp -r "$repo/coreelec/packages" "$ce/packages/addons/addon-depends/coretube-depends"
  ce_env
  local stale
  stale=$(stale_packages)
  if [ -n "$stale" ] && [ "$allow_rebuild" = 0 ]; then
    die "the build folder has these built from other sources; building would replace them:
$stale
Use another --build-dir, or --allow-rebuild to replace them."
  fi
  local p
  for p in $packages; do (cd "$ce" && scripts/build "$p"); done
}

find_engine() {
  local d
  for d in "$out" "$src/out/${stock}_qa" ${legacy:+"$src/out/${legacy}_qa"}; do
    [ -f "$d/app/cobalt/lib/libcobalt.so" ] && { echo "$d"; return; }
  done
}

step_cobalt() {
  # The CoreELEC platforms' args.gn point at the toolchain under /build.
  [ "$build_dir" = /build ] || die "the cobalt step needs the build folder at /build; run with --docker"
  export DEPOT_TOOLS_UPDATE=0 DEPOT_TOOLS_METRICS=0 DEPOT_TOOLS_REPORT_BUILD=0 \
    DEPOT_TOOLS_COLLECT_METRICS=0 GCLIENT_SUPPRESS_GIT_VERSION_WARNING=1
  # depot_tools keeps its caches in HOME; keep them with the checkout this step owns.
  export HOME=$cobalt_dir/home
  mkdir -p "$HOME"
  [ -d "$cobalt_dir/depot_tools/.git" ] ||
    { git clone -q "$DEPOT_TOOLS_URL" "$cobalt_dir/depot_tools"
      git -C "$cobalt_dir/depot_tools" checkout -q "$DEPOT_TOOLS_COMMIT"; }
  export PATH=$cobalt_dir/depot_tools:$ce_build/toolchain/bin:$PATH
  if [ ! -d "$src/.git" ]; then
    fetch_git "$src" "$COBALT_URL" "$COBALT_COMMIT"
    cat > "$cobalt_dir/.gclient" <<EOF
solutions = [{"name": "src", "url": "$COBALT_URL", "managed": False,
              "custom_deps": {}, "custom_vars": {}}]
target_os = ["linux"]
target_cpu = ["x64", "arm"]
EOF
    (cd "$cobalt_dir" && gclient sync --no-history --shallow -j "$jobs")
  fi
  local status=0
  sh "$repo/cobalt/apply.sh" "$src" || status=$?
  [ "$status" = 0 ] || die "cobalt/apply.sh refused $src (see above)"
  (cd "$src" && python3 cobalt/build/gn.py -p "$platform" -C qa --no-rbe &&
    autoninja -C "out/${platform}_qa" -j "$jobs" libloader_app.so starboard/libocdm.so \
      starboard/content/fonts:copy_font_data)
  if [ -z "$(find_engine)" ]; then
    (cd "$src" && python3 build/linux/sysroot_scripts/install-sysroot.py --arch="$sysroot_arch" &&
      python3 cobalt/build/gn.py -p "$stock" -C qa --no-rbe &&
      autoninja -C "out/${stock}_qa" -j "$jobs" cobalt)
  fi
}

step_helper() {
  mkdir -p "$work_dir/$arch"
  TOOLING_DIR=$work_dir/tooling COBALT_SRC=$src bash "$repo/drm/helper/build.sh" "$work_dir/$arch/android-drm-helper"
}

step_zip() {
  local runtime=$repo/drm/runtime/runtime-cube-armv7.tar.gz helper=$work_dir/$arch/android-drm-helper
  local install=$ce_build/install_pkg cc=$ce_build/toolchain/bin/$triple-gcc engine f
  python3 -I "$repo/drm/tools/runtime_bundle.py" check "$runtime"
  engine=$(find_engine)
  [ -n "$engine" ] || die "no Cobalt engine (libcobalt.so) in $src/out (run the cobalt step)"
  for f in "$out/libloader_app.so" "$out/starboard/libocdm.so" "$out/fonts/fonts.xml" "$helper" "$cc"; do
    [ -e "$f" ] || die "missing $f (run the earlier steps)"
  done
  grep -q '<font' "$out/fonts/fonts.xml" || die "empty system font package in $out/fonts"

  # Stage in a temporary folder: this step may write only out/.
  local stage d
  stage=$(mktemp -d)
  trap 'rm -rf "$stage"' RETURN
  d=$stage/browser.tizentube
  mkdir -p "$d/lib/gstreamer-1.0" "$d/cobalt/app/cobalt/lib" "$d/drm"
  cp "$repo/addon/addon.xml" "$repo/addon/default.py" "$d/"
  cp -r "$repo/addon/resources" "$repo/addon/bin" "$d/"
  find "$d" -type d -name __pycache__ -prune -exec rm -rf -- {} +
  cp "$runtime" "$helper" "$d/drm/"
  cp "$install"/coretube-gstreamer-*/usr/lib/gstreamer-1.0/gst-plugin-scanner "$d/bin/"
  # GStreamer: the libraries Cobalt and the plugins link, and all our plugins.
  cp -L "$install"/coretube-gstreamer-*/usr/lib/{libgstreamer,libgstbase,libgstnet}-1.0.so.0 \
        "$install"/coretube-gst-plugins-base-*/usr/lib/libgst{allocators,app,audio,pbutils,riff,rtp,tag,video}-1.0.so.0 \
        "$install"/coretube-gst-plugins-bad-*/usr/lib/libgstcodecparsers-1.0.so.0 \
        "$d/lib/"
  cp "$install"/{coretube-gstreamer,coretube-gst-plugins-base,coretube-gst-plugins-good,coretube-gst-plugins-bad,coretube-gst-libav,coretube-amlvideosink}-*/usr/lib/gstreamer-1.0/*.so \
     "$d/lib/gstreamer-1.0/"
  cp "$out/libloader_app.so" "$d/cobalt/"
  cp "$out/starboard/libocdm.so" "$d/lib/"
  # shellcheck disable=SC2086  # cc_flags is empty or one flag
  "$cc" $cc_flags -O2 -Wall -Werror -o "$d/cobalt/cobalt" "$repo/addon/launcher.c" \
    -L"$d/cobalt" -lloader_app -Wl,-rpath,\$ORIGIN -Wl,-rpath-link,"$d/lib"
  cp -r "$out/fonts" "$d/cobalt/"
  cp "$engine/app/cobalt/lib/libcobalt.so" "$d/cobalt/app/cobalt/lib/"
  cp -r "$engine/app/cobalt/content" "$d/cobalt/app/cobalt/"
  chmod +x "$d"/bin/* "$d/cobalt/cobalt"

  # Never ship DRM state: only the helper and the checked runtime belong in drm/.
  [ -z "$(find "$d" -type d -name android-drm)" ] || die "a DRM profile folder is in the zip stage"
  [ -z "$(find "$d/drm" -type f ! -name android-drm-helper ! -name runtime-cube-armv7.tar.gz)" ] ||
    die "unexpected files in the zip's drm folder: $(ls "$d/drm")"

  mkdir -p "$(dirname "$zip_file")"
  rm -f "$zip_file"
  (cd "$stage" && python3 -m zipfile -c "$zip_file" browser.tizentube)
  echo "engine from: $engine"
  du -h "$zip_file"
}

# --- tests -------------------------------------------------------------------

run_tests() {
  if [ "$docker" = 1 ]; then
    ensure_image coretube-no
    local -a mounts=(-v "$(map_path "$repo")":/coretube)
    local -a args=(test)
    [ -z "$cobalt_dir" ] || { mounts+=(-v "$(map_path "$cobalt_dir")":/cobalt:ro); args+=(--cobalt-dir /cobalt); }
    [ -z "$build_dir" ] || { mounts+=(-v "$(map_path "$build_dir")":/build:ro); args+=(--build-dir /build --arch "$arch"); }
    docker run --rm -e PYTHONDONTWRITEBYTECODE=1 "${mounts[@]}" coretube-no bash /coretube/build-coretube.sh "${args[@]}"
    return
  fi
  sh "$repo/tests/run.sh"
  local scratch
  scratch=$(mktemp -d)
  trap 'rm -rf "$scratch"' RETURN
  if [ -n "$cobalt_dir" ]; then
    local rdk=$src/starboard/contrib/rdk/src/third_party/starboard/rdk/coreelec
    sh "$rdk/ocdm_widevine/tests/run_tests.sh" "$scratch"
    sh "$rdk/tests/run_host_tests.sh" "$scratch"
  fi
  if [ -n "$build_dir" ]; then
    local sink=$repo/coreelec/packages/coretube-amlvideosink/sources
    # shellcheck disable=SC2046  # pkg-config prints several flags
    gcc -std=gnu11 -Wall -Wextra -o "$scratch/test_sink" "$sink/test_sink.c" \
      -I"$ce_build/toolchain/$triple/sysroot/usr/include/amcodec" \
      $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0) \
      -ldl -DPACKAGE='"test"' -DVERSION='"test"'
    "$scratch/test_sink"
  fi
}

# --- main --------------------------------------------------------------------

if [ "$mode" = test ]; then run_tests; exit; fi

steps=${only:-$all_steps}
if [ "$dry_run" = 1 ]; then
  echo "arch $arch: CoreELEC $ce_device $ce_url @ ${ce_commit:0:12}, image $image"
  echo "step sources: helper tooling (as owner of $(owner_folder sources); writes $work_dir/tooling)"
  echo "step packages: CoreELEC checkout $ce, $packages, stamps checked, --allow-rebuild=$allow_rebuild (as owner of $(owner_folder packages); writes $ce_build)"
  echo "step cobalt: $src at ${COBALT_COMMIT:0:12} + cobalt/patches, $platform qa, engine from $platform, $stock${legacy:+ or $legacy} (as owner of $(owner_folder cobalt); writes $cobalt_dir)"
  echo "step helper: android-drm-helper (as owner of $(owner_folder helper); writes $work_dir/$arch)"
  echo "step zip: $zip_file (as owner of $(owner_folder zip); writes $repo/out)"
  echo "jobs: $jobs"
  exit 0
fi

mkdir -p "$work_dir"
for step in $steps; do
  if [ "$docker" = 1 ]; then
    ensure_image "$image"
    in_docker "$step"
  else
    echo "== $step"
    "step_$step"
  fi
done
echo "done: $steps"
