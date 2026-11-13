#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""TV remote over HDMI-CEC for Cobalt: a virtual keyboard fed by cec-client.

Usage: cec-remote.py KODI_CEC_SETTINGS
Creates the keyboard, then returns while a child keeps running: Cobalt finds
input devices only when it starts. Anything missing (CEC off in Kodi, no
cec-client, no uinput) only disables the remote; CoreTube starts regardless.
"""

import fcntl
import os
import select
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "resources" / "lib"))
import cec_remote

# linux/uinput.h and linux/input-event-codes.h
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_DEV_CREATE = 0x5501
EV_SYN, EV_KEY, SYN_REPORT = 0, 1, 0
BUS_VIRTUAL = 0x06
ABS_CNT = 64


def create_keyboard():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
        # Cobalt takes only devices with a space key as keyboards.
        for key in {cec_remote.KEY_SPACE, *cec_remote.BUTTONS.values()}:
            fcntl.ioctl(fd, UI_SET_KEYBIT, key)
        # struct uinput_user_dev: name, input_id, ff_effects_max, 4 abs arrays.
        os.write(
            fd,
            struct.pack("80s4HI", b"CoreTube CEC remote", BUS_VIRTUAL, 0, 0, 1, 0)
            + bytes(4 * 4 * ABS_CNT),
        )
        fcntl.ioctl(fd, UI_DEV_CREATE)
    except OSError:
        os.close(fd)
        raise
    return fd


def send(fd, events):
    for key, value in events:
        # struct input_event: a native timeval (the kernel stamps it), type, code, value.
        os.write(
            fd,
            struct.pack("@llHHi", 0, 0, EV_KEY, key, value)
            + struct.pack("@llHHi", 0, 0, EV_SYN, SYN_REPORT, 0),
        )


def listen(fd, name):
    client = subprocess.Popen(
        ["cec-client", "-t", "p", "-o", name, "-d", "8"],
        stdin=subprocess.PIPE,  # kept open: cec-client reads commands from it
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )
    print(f"cec remote: listening as '{name}'", flush=True)
    keys = cec_remote.Keys()
    while True:
        ready, _, _ = select.select(
            [client.stdout], [], [], keys.timeout(time.monotonic())
        )
        now = time.monotonic()
        if not ready:
            send(fd, keys.tick(now))
            continue
        line = client.stdout.readline()
        if not line:
            break
        frame = cec_remote.received_frame(line)
        if frame:
            send(fd, keys.frame(frame, now))
    send(fd, keys.tick(float("inf")))
    print(f"cec remote: cec-client ended ({client.wait()})", flush=True)


def main(settings):
    enabled, name = cec_remote.kodi_settings(settings)
    if not enabled:
        print("cec remote: off (CEC is off in Kodi's settings)", flush=True)
        return 0
    if not shutil.which("cec-client"):
        print("cec remote: off (no cec-client)", flush=True)
        return 0
    try:
        fd = create_keyboard()
    except OSError as error:
        print(f"cec remote: off (no virtual keyboard: {error})", flush=True)
        return 0
    if os.fork():
        return 0
    try:
        listen(fd, name)
    except OSError as error:
        print(f"cec remote: stopped ({error})", flush=True)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
