# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
"""The systemd unit Cobalt runs in."""


def test_no_core_dumps(start_command):
  # A crash at exit would otherwise write a dump of several hundred MB to the eMMC
  # (CoreELEC keeps cores, unlimited) and hold up Kodi's return for seconds.
  assert '--property=LimitCORE=0' in start_command({})
