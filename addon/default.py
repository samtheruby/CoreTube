# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

import json
import os
import subprocess
import sys

import xbmc
import xbmcaddon
import xbmcgui

addon = xbmcaddon.Addon()
addon_dir = addon.getAddonInfo('path')
sys.path.insert(0, os.path.join(addon_dir, 'resources', 'lib'))

import display_modes

DISP_CAP = '/sys/class/amhdmitx/amhdmitx0/disp_cap'
# Cobalt identifies as a Fire TV Cube (2019): YouTube buffers much better for it
# than for Cobalt's own user agent. TIZENTUBE_USER_AGENT in the env file overrides it.
FIRE_TV_USER_AGENT = ('Mozilla/5.0 (RDK; Linux 4.9.269) Cobalt/27.lts.10.1041863-qa (unlike Gecko) '
                      'v8/13.8.258.54-jit gles Evergreen/1.0.0 Evergreen-Lite Evergreen-Uncompressed '
                      'Starboard/18, Amazon_ATV_S922X_2019/AFTR (Amazon, AFTR, Wireless)')


def read_disp_cap():
  try:
    with open(DISP_CAP) as f:
      return f.read()
  except OSError:
    return ''


def kodi_setting(setting, default):
  request = {'jsonrpc': '2.0', 'id': 1, 'method': 'Settings.GetSettingValue',
             'params': {'setting': setting}}
  reply = json.loads(xbmc.executeJSONRPC(json.dumps(request)))
  return reply.get('result', {}).get('value', default)


def kodi_whitelist():
  return kodi_setting('videoscreen.whitelist', [])


def switch_delay_ms():
  """Kodi's "Delay after change of refresh rate" (tenths of a second) in ms, for Cobalt."""
  tenths = kodi_setting('videoscreen.delayrefreshchange', 0)
  return tenths * 100 if isinstance(tenths, int) and tenths > 0 else 0


def allowed_modes():
  """TIZENTUBE_DISPLAY_MODES for Cobalt, from the add-on's auto frame rate setting."""
  if addon.getSetting('afr_modes') == 'custom':
    return display_modes.from_mode_names(addon.getSetting('custom_modes').replace(',', ' ').split())
  # Kodi switches to any mode the TV takes when its whitelist is empty; so does TizenTube.
  return (display_modes.from_kodi_whitelist(kodi_whitelist())
          or display_modes.from_mode_names(display_modes.display_modes(read_disp_cap())))


def choose_modes():
  """The "Choose custom display modes" button: pick from the modes the TV takes."""
  names = display_modes.display_modes(read_disp_cap())
  if not names:
    xbmcgui.Dialog().ok(addon.getAddonInfo('name'), f'Cannot read the display modes from {DISP_CAP}.')
    return
  current = addon.getSetting('custom_modes').replace(',', ' ').split()
  picked = xbmcgui.Dialog().multiselect(addon.getLocalizedString(30018), names,
                                        preselect=[i for i, n in enumerate(names) if n in current])
  if picked is not None:
    addon.setSetting('custom_modes', ','.join(names[i] for i in picked))
  addon.openSettings()


def start():
  max_height = addon.getSetting('max_resolution')
  env = {'TIZENTUBE_DISPLAY_MODES': allowed_modes(),
         'TIZENTUBE_MAX_VIDEO_HEIGHT': '' if max_height == 'auto' else max_height,
         # Playback waits this long after an auto frame rate mode switch, as in Kodi.
         'TIZENTUBE_SWITCH_DELAY_MS': str(switch_delay_ms()),
         'TIZENTUBE_USER_AGENT': FIRE_TV_USER_AGENT}
  # Cobalt needs the framebuffer, so Kodi stops while it runs. A separate systemd
  # unit keeps Cobalt alive when Kodi stops, and ExecStopPost brings Kodi back
  # however Cobalt ends (exit, crash or `systemctl stop tizentube`).
  result = subprocess.run(['systemd-run', '--unit=tizentube', '--collect',
                           f'--property=ExecStopPost=/bin/sh {os.path.join(addon_dir, "bin", "tizentube-stop")}',
                           '--property=ExecStopPost=/usr/bin/systemctl --no-block start kodi',
                           *[f'--setenv={k}={v}' for k, v in env.items()],
                           '/bin/sh', os.path.join(addon_dir, 'bin', 'tizentube-start')],
                          check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
  if result.returncode != 0:
    xbmc.log(f'## TizenTube Addon Error: systemd-run failed: {result.stdout.strip()}', xbmc.LOGERROR)


if sys.argv[1:] == ['choose_modes']:
  choose_modes()
else:
  start()
