# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

import runpy
import subprocess
import sys
import types
from pathlib import Path

import pytest

ADDON = Path(__file__).resolve().parent.parent


@pytest.fixture
def start_env(monkeypatch):
  """start_env(settings): the environment default.py gives Cobalt's unit, with these add-on settings."""
  def run(settings):
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
  return run
