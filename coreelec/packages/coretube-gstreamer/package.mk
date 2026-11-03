# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

PKG_NAME="coretube-gstreamer"
PKG_VERSION="1.28.8"
PKG_SHA256="baa8e86985ad6417cc0ad6e7b79ff2884c6779c8b1c7a6240e7121822c86e524"
PKG_LICENSE="LGPL-2.1-or-later"
PKG_SITE="https://gstreamer.freedesktop.org"
PKG_URL="https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_TARGET="toolchain glib"
PKG_LONGDESC="GStreamer core for the TizenTube add-on (the image's gstreamer has no plugin registry)."

# shortcut: installs into the shared build sysroot, like the image's packages; use a
# dedicated sysroot before building an image that needs its own stripped GStreamer.
PKG_MESON_OPTS_TARGET="-Dauto_features=disabled \
                       -Dtools=enabled"
