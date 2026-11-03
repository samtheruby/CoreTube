# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

# Included by every CoreTube package. They install under a private prefix inside
# the shared CoreELEC sysroot, so the image's own GStreamer there (headers,
# libraries, tools) stays untouched, and they find each other there first.
CORETUBE_PREFIX="/usr/lib/coretube"
CORETUBE_MESON_OPTS="--prefix=${CORETUBE_PREFIX} \
                     --bindir=bin \
                     --sbindir=bin \
                     --libdir=lib \
                     --libexecdir=libexec \
                     --includedir=include \
                     --datadir=share \
                     --sysconfdir=etc \
                     --localstatedir=var \
                     -Dpkg_config_path=${SYSROOT_PREFIX}${CORETUBE_PREFIX}/lib/pkgconfig"
# A change here rebuilds every package that includes it.
PKG_NEED_UNPACK+=" $(get_pkg_directory coretube-gstreamer)/coretube.mk"
