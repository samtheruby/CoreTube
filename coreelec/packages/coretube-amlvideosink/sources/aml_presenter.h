/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026-present Team CoreELEC (https://coreelec.org) */

/* Shows the frames the decoder puts out on amlvideo, one vsync at a time: a thread
 * wakes on each vsync, takes the decoded frames, and hands back the one due (shown at
 * the next vsync) and the late ones (dropped). */

#ifndef AML_PRESENTER_H
#define AML_PRESENTER_H

#include <gst/gst.h>

/* Device operations; the host test supplies fakes. */
typedef struct {
  int (*poll_vsync)(void *ctx, int timeout_ms);             /* 1 vsync, 0 timeout, < 0 error */
  int (*dequeue)(void *ctx, guint *index, guint64 *pts_us); /* 0, or -EAGAIN when none is ready */
  int (*queue)(void *ctx, guint index, gboolean drop);      /* show at the next vsync, or drop */
  gint64 (*running_us)(void *ctx);                          /* pipeline running time; -1 without a clock */
  gint64 (*now_us)(void *ctx);                              /* monotonic time */
} AmlPresenterOps;

typedef struct AmlPresenter AmlPresenter;

AmlPresenter *aml_presenter_new(const AmlPresenterOps *ops, void *ctx);
/* Frame timestamps (stream time in us + 1, 0 = none) map to running time by this segment. */
void aml_presenter_set_segment(AmlPresenter *p, const GstSegment *segment);
/* Starts the thread; also after a flush. */
void aml_presenter_start(AmlPresenter *p);
/* Stops the thread and hands back every frame it holds, dropped. */
void aml_presenter_flush(AmlPresenter *p);
/* Paused, only the first frame after a start shows (the preroll picture). */
void aml_presenter_set_playing(AmlPresenter *p, gboolean playing);
/* Waits until the frame with this timestamp (or a later one) was shown or dropped, at
 * most timeout_us. Returns the timestamp of the last frame shown, 0 for none. */
guint64 aml_presenter_wait_shown(AmlPresenter *p, guint64 pts_us, gint64 timeout_us);
/* Called from the thread on the first frame shown after each start. */
void aml_presenter_set_shown_callback(AmlPresenter *p, void (*callback)(gpointer), gpointer data);
/* Flushes first. */
void aml_presenter_free(AmlPresenter *p);

#endif
