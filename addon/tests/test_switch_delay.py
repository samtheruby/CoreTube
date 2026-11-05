# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
"""Kodi's "Delay after change of refresh rate" reaches Cobalt, in milliseconds."""

import xml.etree.ElementTree as ET
from pathlib import Path

ADDON = Path(__file__).resolve().parent.parent
DELAY = 'videoscreen.delayrefreshchange'


def test_kodi_delay_in_ms(start_env):
  assert start_env({}, {DELAY: 25})['TIZENTUBE_SWITCH_DELAY_MS'] == '2500'
  assert start_env({}, {DELAY: 0})['TIZENTUBE_SWITCH_DELAY_MS'] == '0'


def test_no_usable_kodi_delay(start_env):
  assert start_env({})['TIZENTUBE_SWITCH_DELAY_MS'] == '0'           # not answered
  assert start_env({}, {DELAY: -5})['TIZENTUBE_SWITCH_DELAY_MS'] == '0'
  assert start_env({}, {DELAY: 'x'})['TIZENTUBE_SWITCH_DELAY_MS'] == '0'


def test_previews_cannot_be_turned_on(start_env):
  settings = ET.parse(ADDON / 'resources' / 'settings.xml')
  assert settings.find(".//setting[@id='video_previews']") is None
  assert not any(k.startswith('TIZENTUBE_VIDEO_PREVIEWS') for k in start_env({'video_previews': 'true'}))
