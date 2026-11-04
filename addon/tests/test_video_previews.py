# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
"""Home screen video previews: an add-on setting, off by default, sent to Cobalt."""

import re
import xml.etree.ElementTree as ET
from pathlib import Path

ADDON = Path(__file__).resolve().parent.parent


def test_previews_off_by_default():
  setting = ET.parse(ADDON / 'resources' / 'settings.xml').find(".//setting[@id='video_previews']")
  assert setting.get('type') == 'boolean'
  assert setting.findtext('default') == 'false'
  # The help text warns about the flashes.
  strings = (ADDON / 'resources' / 'language' / 'resource.language.en_gb' / 'strings.po').read_text()
  help_text = re.search(rf'msgctxt "#{setting.get("help")}"\nmsgid "([^"]+)"', strings).group(1)
  assert 'flash' in help_text


def test_setting_reaches_cobalt(start_env):
  assert start_env({})['TIZENTUBE_VIDEO_PREVIEWS'] == '0'
  assert start_env({'video_previews': 'false'})['TIZENTUBE_VIDEO_PREVIEWS'] == '0'
  assert start_env({'video_previews': 'true'})['TIZENTUBE_VIDEO_PREVIEWS'] == '1'
