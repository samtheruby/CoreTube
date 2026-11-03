# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

PKG_NAME="coretube-gst-plugins-bad"
PKG_VERSION="1.28.8"
PKG_SHA256="670d888bde8242579b0d8e91f60cf337aa7be20ca7c315ebfbdd6e5a9218d104"
PKG_LICENSE="LGPL-2.1-or-later"
PKG_SITE="https://gstreamer.freedesktop.org"
PKG_URL="https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_TARGET="toolchain coretube-gst-plugins-base"
PKG_LONGDESC="GStreamer bad plugins for the TizenTube add-on."
. $(get_pkg_directory coretube-gstreamer)/coretube.mk

# H.264/H.265/VP9 parsers to feed the hardware decoder whole frames, and the
# debug utilities (fakevideosink for players without video).
PKG_MESON_OPTS_TARGET="${CORETUBE_MESON_OPTS} \
                       -Dauto_features=disabled \
                       -Ddebugutils=enabled \
                       -Dvideoparsers=enabled"
