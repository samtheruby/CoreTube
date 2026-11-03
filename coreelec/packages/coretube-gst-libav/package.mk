# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

PKG_NAME="coretube-gst-libav"
PKG_VERSION="1.28.8"
PKG_SHA256="98ad3d7f8932c495e268708903f1c3388d5f26a0db34727a9f861ee23b724177"
PKG_LICENSE="LGPL-2.1-or-later"
PKG_SITE="https://gstreamer.freedesktop.org"
PKG_URL="https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_TARGET="toolchain ffmpeg coretube-gst-plugins-base"
PKG_LONGDESC="GStreamer plugin for the image's FFmpeg, used by the TizenTube add-on to decode audio."
. $(get_pkg_directory coretube-gstreamer)/coretube.mk

PKG_MESON_OPTS_TARGET="${CORETUBE_MESON_OPTS} \
                       -Ddoc=disabled \
                       -Dtests=disabled"
