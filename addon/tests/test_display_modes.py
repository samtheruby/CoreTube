# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'resources' / 'lib'))

import display_modes  # noqa: E402

# Excerpt of the test box's disp_cap (S922Z on a 4K TV).
DISP_CAP = '1080i60hz\n1080p60hz\n720p50hz\n2160p24hz\n2160p50hz\n2160p60hz*\nsmpte24hz\n640x480p60hz\n'


def test_parse_mode():
  assert display_modes.parse_mode('2160p60hz') == (3840, 2160, 60, True)
  assert display_modes.parse_mode('1080i50hz') == (1920, 1080, 50, False)
  assert display_modes.parse_mode('smpte24hz') == (4096, 2160, 24, True)
  assert display_modes.parse_mode('640x480p60hz') == (640, 480, 60, True)
  assert display_modes.parse_mode('576p50hz') == (720, 576, 50, True)
  assert display_modes.parse_mode('nonsense') is None


def test_display_modes_drop_interlaced_and_mark():
  assert display_modes.display_modes(DISP_CAP) == [
      '1080p60hz', '720p50hz', '2160p24hz', '2160p50hz', '2160p60hz', 'smpte24hz', '640x480p60hz']


def test_from_mode_names_adds_fractional_rates():
  assert display_modes.from_mode_names(['2160p24hz', '2160p50hz', '1080i60hz']) == \
      '3840x2160@24 3840x2160@23.976 3840x2160@50'


def test_from_kodi_whitelist():
  whitelist = ['0384002160060.00000pstd', '0384002160023.97602pstd', '0192001080050.00000istd', 'bad']
  assert display_modes.from_kodi_whitelist(whitelist) == '3840x2160@60 3840x2160@23.976'
