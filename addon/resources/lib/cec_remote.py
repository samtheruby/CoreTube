# SPDX-License-Identifier: BSD-3-Clause
"""TV remote buttons over HDMI-CEC, as key events for Cobalt.

Kodi's libCEC closes while CoreTube runs. cec-client (libCEC) takes over the
CEC bus; this reads the frames it logs and turns the TV's User Control
Pressed/Released messages into key presses (see bin/cec-remote.py).
"""

import re
from xml.etree import ElementTree

# linux/input-event-codes.h
KEY_ESC = 1
KEY_ENTER = 28
KEY_SPACE = 57
KEY_UP = 103
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_DOWN = 108
KEY_NEXTSONG = 163
KEY_PLAYPAUSE = 164
KEY_PREVIOUSSONG = 165
KEY_STOPCD = 166

USER_CONTROL_PRESSED = 0x44
USER_CONTROL_RELEASED = 0x45

# CEC button (libCEC's cec_user_control_code) to the key Cobalt maps for it.
# Cobalt has no separate play or pause key: both toggle, as the YouTube app does.
BUTTONS = {
    0x00: KEY_ENTER,  # select
    0x01: KEY_UP,
    0x02: KEY_DOWN,
    0x03: KEY_LEFT,
    0x04: KEY_RIGHT,
    0x0D: KEY_ESC,  # exit
    0x91: KEY_ESC,  # return (Samsung)
    0x44: KEY_PLAYPAUSE,  # play
    0x45: KEY_STOPCD,
    0x46: KEY_PLAYPAUSE,  # pause
    0x4B: KEY_NEXTSONG,  # forward
    0x4C: KEY_PREVIOUSSONG,  # backward
    0x60: KEY_PLAYPAUSE,  # play function
    0x61: KEY_PLAYPAUSE,  # pause-play function
}

# CEC: a held button repeats its press within this; silence means released.
RELEASE_AFTER = 0.55

_TRAFFIC = re.compile(r"TRAFFIC: \[[ \d]+\]\t>> [0-9a-f]{2}((?::[0-9a-f]{2})+)$")


def received_frame(line):
    """(opcode, parameters) of a frame libCEC logs as received, else None."""
    match = _TRAFFIC.match(line.rstrip("\n"))
    if not match:
        return None
    data = [int(part, 16) for part in match.group(1)[1:].split(":")]
    return data[0], data[1:]


class Keys:
    """The key held down by the TV remote. Methods return (key, value) events:
    1 pressed, 2 repeated, 0 released."""

    def __init__(self):
        self._key = None
        self._last = 0.0

    def frame(self, frame, now):
        opcode, parameters = frame
        if opcode == USER_CONTROL_RELEASED:
            return self._release()
        if opcode != USER_CONTROL_PRESSED or not parameters:
            return []
        key = BUTTONS.get(parameters[0])
        if key is None:
            return []
        self._last = now
        if key == self._key:
            return [(key, 2)]
        events = self._release()
        self._key = key
        return events + [(key, 1)]

    def timeout(self, now):
        """Seconds until a held key counts as released; None when none is held."""
        if self._key is None:
            return None
        return max(0.0, round(self._last + RELEASE_AFTER - now, 6))

    def tick(self, now):
        if self._key is not None and now >= self._last + RELEASE_AFTER:
            return self._release()
        return []

    def _release(self):
        if self._key is None:
            return []
        key, self._key = self._key, None
        return [(key, 0)]


def kodi_settings(path):
    """(enabled, device name) from Kodi's CEC adapter settings; Kodi's defaults
    when the file is missing or unreadable."""
    values = {}
    try:
        for setting in ElementTree.parse(path).getroot().iter("setting"):
            values[setting.get("id")] = setting.get("value")
    except (OSError, ElementTree.ParseError):
        pass
    return values.get("enabled", "1") != "0", values.get("device_name") or "CoreELEC"
