# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
"""Cobalt identifies as a Fire TV Cube by default: YouTube buffers much better for it."""


def test_fire_tv_user_agent_by_default(start_env):
  agent = start_env({})['TIZENTUBE_USER_AGENT']
  assert agent.startswith('Mozilla/5.0 (RDK; Linux ')
  assert agent.endswith(' Amazon_ATV_S922X_2019/AFTR (Amazon, AFTR, Wireless)')
