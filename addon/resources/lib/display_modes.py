# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

"""Display modes TizenTube's auto frame rate may switch to.

Cobalt reads them from TIZENTUBE_DISPLAY_MODES as "WIDTHxHEIGHT@RATE" items
separated by spaces (see coreelec_display.cc in the Cobalt tree).
"""

import re

# Amlogic runs 24, 30, 60 and 120 Hz modes 1000/1001 slower when asked (frac_rate_policy).
FRACTIONAL_RATES = (24, 30, 60, 120)

_MODE = re.compile(r'(?:(\d+)x)?(\d+)([pi])(\d+)hz')
_SMPTE = re.compile(r'smpte(\d+)hz')


def parse_mode(name):
  """(width, height, rate, progressive) of an Amlogic mode name such as "2160p60hz", or None."""
  m = _SMPTE.fullmatch(name)
  if m:
    return 4096, 2160, int(m.group(1)), True
  m = _MODE.fullmatch(name)
  if not m:
    return None
  height = int(m.group(2))
  if m.group(1):
    width = int(m.group(1))
  elif height in (480, 576):
    width = 720
  else:
    width = height * 16 // 9
  return width, height, int(m.group(4)), m.group(3) == 'p'


def display_modes(disp_cap):
  """Progressive mode names from disp_cap's text (the current one is marked "*")."""
  names = [line.strip().rstrip('*') for line in disp_cap.splitlines()]
  return [n for n in names if (mode := parse_mode(n)) and mode[3]]


def _item(width, height, rate):
  return f'{width}x{height}@{rate:.3f}'.rstrip('0').rstrip('.')


def from_mode_names(names):
  """Allowed modes for Amlogic mode names; 24/30/60/120 Hz ones also allow their 1000/1001 rate."""
  items = []
  for name in names:
    mode = parse_mode(name)
    if not mode or not mode[3]:
      continue
    width, height, rate, _ = mode
    items.append(_item(width, height, rate))
    if rate in FRACTIONAL_RATES:
      items.append(_item(width, height, rate * 1000 / 1001))
  return ' '.join(items)


def from_kodi_whitelist(whitelist):
  """Allowed modes for Kodi's videoscreen.whitelist entries ("0384002160023.97602pstd")."""
  items = []
  for entry in whitelist:
    m = re.fullmatch(r'(\d{5})(\d{5})(\d+\.\d+)([pi])\w*', entry)
    if m and m.group(4) == 'p':
      items.append(_item(int(m.group(1)), int(m.group(2)), float(m.group(3))))
  return ' '.join(items)
