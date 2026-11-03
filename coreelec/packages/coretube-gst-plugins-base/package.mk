# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

PKG_NAME="coretube-gst-plugins-base"
PKG_VERSION="1.28.8"
PKG_SHA256="ade70e3381fcbb024f1f20aa9ede9b5654aeed39d5bd70b1a98f53015e501117"
PKG_LICENSE="LGPL-2.1-or-later"
PKG_SITE="https://gstreamer.freedesktop.org"
PKG_URL="https://gstreamer.freedesktop.org/src/gst-plugins-base/gst-plugins-base-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_TARGET="toolchain alsa-lib coretube-gstreamer"
PKG_LONGDESC="GStreamer base plugins and libraries for the TizenTube add-on."

# Playback (playbin), ALSA output and the audio converters; video is decoded and
# shown by the Amlogic hardware, so no video converters or GL.
PKG_MESON_OPTS_TARGET="-Dauto_features=disabled \
                       -Dalsa=enabled \
                       -Dapp=enabled \
                       -Daudioconvert=enabled \
                       -Daudioresample=enabled \
                       -Dgio=enabled \
                       -Dgio-typefinder=enabled \
                       -Dplayback=enabled \
                       -Dtypefind=enabled \
                       -Dvolume=enabled"
