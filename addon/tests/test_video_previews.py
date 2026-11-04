# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
"""Home screen video previews: an add-on setting, off by default, sent to Cobalt."""

import re
import runpy
import subprocess
import sys
import types
import xml.etree.ElementTree as ET
from pathlib import Path

ADDON = Path(__file__).resolve().parent.parent


def start_env(monkeypatch, settings):
  """The environment default.py gives Cobalt's unit, with these add-on settings."""
  addon = types.SimpleNamespace(
      getAddonInfo=lambda key: str(ADDON),
      getSetting=lambda key: settings.get(key, ''),
      getSettingBool=lambda key: settings.get(key) == 'true')
  monkeypatch.setitem(sys.modules, 'xbmcaddon', types.SimpleNamespace(Addon=lambda: addon))
  monkeypatch.setitem(sys.modules, 'xbmc', types.SimpleNamespace(
      executeJSONRPC=lambda request: '{"result": {"value": []}}', log=print, LOGERROR=0))
  monkeypatch.setitem(sys.modules, 'xbmcgui', types.SimpleNamespace())
  monkeypatch.setattr(sys, 'argv', ['default.py'])
  calls = []
  monkeypatch.setattr(subprocess, 'run', lambda args, **kw: calls.append(args) or
                      types.SimpleNamespace(returncode=0, stdout=''))
  runpy.run_path(str(ADDON / 'default.py'))
  return dict(a[len('--setenv='):].split('=', 1) for a in calls[0] if a.startswith('--setenv='))


def test_previews_off_by_default():
  setting = ET.parse(ADDON / 'resources' / 'settings.xml').find(".//setting[@id='video_previews']")
  assert setting.get('type') == 'boolean'
  assert setting.findtext('default') == 'false'
  # The help text warns about the flashes.
  strings = (ADDON / 'resources' / 'language' / 'resource.language.en_gb' / 'strings.po').read_text()
  help_text = re.search(rf'msgctxt "#{setting.get("help")}"\nmsgid "([^"]+)"', strings).group(1)
  assert 'flash' in help_text


def test_setting_reaches_cobalt(monkeypatch):
  assert start_env(monkeypatch, {})['TIZENTUBE_VIDEO_PREVIEWS'] == '0'
  assert start_env(monkeypatch, {'video_previews': 'false'})['TIZENTUBE_VIDEO_PREVIEWS'] == '0'
  assert start_env(monkeypatch, {'video_previews': 'true'})['TIZENTUBE_VIDEO_PREVIEWS'] == '1'
