# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)

"""pause-test.py: exit 0 only if the default ALSA device plays on after a pause.

Some kernels report hardware pause but never start again after it. The test
plays 150 ms of silence, pauses, resumes and checks the queued audio drains.
Anything else (an error, no drain, or a hang cut off by the alarm) exits non-zero,
and the browser then stops and restarts the device instead of pausing it.
"""
import ctypes
import signal
import sys
import time

signal.alarm(2)  # a broken resume can block the device; never hold up the browser

asound = ctypes.CDLL('libasound.so.2')
pcm = ctypes.c_void_p()
avail, delay = ctypes.c_long(), ctypes.c_long()


def queued():
  if asound.snd_pcm_avail_delay(pcm, ctypes.byref(avail), ctypes.byref(delay)) < 0:
    sys.exit(1)
  return delay.value


if asound.snd_pcm_open(ctypes.byref(pcm), b'default', 0, 0) < 0:
  sys.exit(1)
# S16_LE, interleaved, stereo, 48 kHz, resampling allowed, 100 ms buffer
if asound.snd_pcm_set_params(pcm, 2, 3, 2, 48000, 1, 100000) < 0:
  sys.exit(1)
silence = ctypes.create_string_buffer(480 * 4)
for _ in range(15):  # 15 x 10 ms
  if asound.snd_pcm_writei(pcm, silence, 480) < 0:
    sys.exit(1)
if asound.snd_pcm_pause(pcm, 1) < 0:
  sys.exit(1)
time.sleep(0.05)
if asound.snd_pcm_pause(pcm, 0) < 0:
  sys.exit(1)
before = queued()
time.sleep(0.05)
works = before > 0 and queued() < before
asound.snd_pcm_drop(pcm)
asound.snd_pcm_close(pcm)
sys.exit(0 if works else 1)
