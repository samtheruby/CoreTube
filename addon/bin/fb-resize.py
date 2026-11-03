# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

"""fb-resize.py WIDTH HEIGHT [BUFFERS]: size fb0 to WIDTH x HEIGHT with BUFFERS buffers (default 2).

Kodi does the same for its GUI. The Mali driver places its two EGL buffers
by the framebuffer size, so it must match the page.
"""
import fcntl
import struct
import sys

FBIOGET_VSCREENINFO = 0x4600
FBIOPUT_VSCREENINFO = 0x4601
VAR_SIZE = 160  # sizeof(struct fb_var_screeninfo)

width, height = int(sys.argv[1]), int(sys.argv[2])
buffers = int(sys.argv[3]) if len(sys.argv) > 3 else 2
with open('/dev/fb0', 'r+b', buffering=0) as fb:
  var = bytearray(fcntl.ioctl(fb, FBIOGET_VSCREENINFO, bytes(VAR_SIZE)))
  # xres, yres, xres_virtual, yres_virtual, xoffset, yoffset
  struct.pack_into('6I', var, 0, width, height, width, buffers * height, 0, 0)
  fcntl.ioctl(fb, FBIOPUT_VSCREENINFO, var)
