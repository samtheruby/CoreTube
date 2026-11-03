/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026-present Team CoreELEC (https://coreelec.org) */

/* Chooses which decoded frame to show on each vsync, as pannal's Kodi does. */

#ifndef AML_PACER_H
#define AML_PACER_H

#include <glib.h>

/* Most frames held at once; more than amlvideo has buffers. */
#define AML_PACER_MAX_FRAMES 16
/* Vsync intervals the period is measured over. */
#define AML_PACER_VSYNC_SAMPLES 8
/* Vsync period until enough intervals are known (60 Hz). */
#define AML_PACER_DEFAULT_VSYNC_US 16667

typedef struct {
  guint index;       /* amlvideo buffer index */
  gint64 running_us; /* when the frame is due, in pipeline running time; < 0: before the segment */
} AmlFrame;

typedef struct {
  gint show;                          /* index to show at the next vsync, or -1 */
  guint drop[AML_PACER_MAX_FRAMES];   /* indexes to hand back unshown */
  guint n_drop;
} AmlPick;

/* queued: frames in decode output order (at most AML_PACER_MAX_FRAMES); target_us: running
 * time at which a frame queued now appears. The frames in show and drop leave the queue. */
AmlPick aml_pacer_pick(const AmlFrame *queued, guint n, gint64 target_us, gint64 vsync_us);

/* Median of the last AML_PACER_VSYNC_SAMPLES intervals; the default with fewer. */
gint64 aml_pacer_vsync_us(const gint64 *intervals_us, guint n);

#endif
