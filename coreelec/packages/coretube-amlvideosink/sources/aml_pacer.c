/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026-present Team CoreELEC (https://coreelec.org) */

#include <stdlib.h>
#include <string.h>

#include "aml_pacer.h"

/* Per vsync: frames before the segment go; of the frames due by the middle of the
 * next vsync the last one shows and the earlier ones go; none due, the screen keeps
 * its frame. This gives 3:2 for 24 fps on 60 Hz and never moves a clock. */
AmlPick aml_pacer_pick(const AmlFrame *queued, guint n, gint64 target_us, gint64 vsync_us)
{
  AmlPick pick = {.show = -1};
  gint due = -1;

  g_return_val_if_fail(n <= AML_PACER_MAX_FRAMES, pick);
  for (guint i = 0; i < n; i++) {
    if (queued[i].running_us < 0) {
      pick.drop[pick.n_drop++] = queued[i].index;
    } else if (queued[i].running_us <= target_us + vsync_us / 2) {
      if (due >= 0)
        pick.drop[pick.n_drop++] = queued[due].index;
      due = (gint)i;
    }
  }
  if (due >= 0)
    pick.show = (gint)queued[due].index;
  return pick;
}

static int compare_gint64(const void *a, const void *b)
{
  gint64 x = *(const gint64 *)a, y = *(const gint64 *)b;

  return (x > y) - (x < y);
}

gint64 aml_pacer_vsync_us(const gint64 *intervals_us, guint n)
{
  gint64 sorted[AML_PACER_VSYNC_SAMPLES];

  if (n < AML_PACER_VSYNC_SAMPLES)
    return AML_PACER_DEFAULT_VSYNC_US;
  memcpy(sorted, intervals_us + n - AML_PACER_VSYNC_SAMPLES, sizeof(sorted));
  qsort(sorted, AML_PACER_VSYNC_SAMPLES, sizeof(sorted[0]), compare_gint64);
  return (sorted[AML_PACER_VSYNC_SAMPLES / 2 - 1] + sorted[AML_PACER_VSYNC_SAMPLES / 2]) / 2;
}
