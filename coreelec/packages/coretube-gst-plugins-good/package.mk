# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

PKG_NAME="coretube-gst-plugins-good"
PKG_VERSION="1.28.8"
PKG_SHA256="a7670ed900d3d03be744d25c1873b239896e9a82ce6d17f8febc6fffbaee9521"
PKG_LICENSE="LGPL-2.1-or-later"
PKG_SITE="https://gstreamer.freedesktop.org"
PKG_URL="https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_TARGET="toolchain pulseaudio coretube-gst-plugins-base"
PKG_LONGDESC="GStreamer good plugins for the TizenTube add-on."

# MP4 and WebM demuxers, audio parsers, and automatic audio output (PulseAudio,
# which also reaches Bluetooth headphones, else ALSA).
PKG_MESON_OPTS_TARGET="-Dauto_features=disabled \
                       -Daudioparsers=enabled \
                       -Dautodetect=enabled \
                       -Did3demux=enabled \
                       -Disomp4=enabled \
                       -Dmatroska=enabled \
                       -Dpulse=enabled"
