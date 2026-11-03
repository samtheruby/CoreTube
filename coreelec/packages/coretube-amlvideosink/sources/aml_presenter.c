/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026-present Team CoreELEC (https://coreelec.org) */

#include "aml_presenter.h"
#include "aml_pacer.h"

GST_DEBUG_CATEGORY_STATIC(aml_presenter_debug);
#undef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT aml_presenter_debug

/* Longest wait for a vsync; it also bounds how long a flush waits for the thread. */
#define POLL_TIMEOUT_MS 50

typedef struct {
  guint index;
  guint64 pts_us;
  gint64 running_us;
} HeldFrame;

struct AmlPresenter {
  const AmlPresenterOps *ops;
  void *ctx;
  GThread *thread;
  gint stopping; /* atomic: the thread must end */

  GMutex lock; /* guards everything below */
  GCond handled_cond;
  gboolean running; /* started and not flushed */
  GstSegment segment;
  gboolean playing;
  HeldFrame held[AML_PACER_MAX_FRAMES];
  guint n_held;
  gboolean shown_since_start;
  GstClockTime last_stream; /* stream time of the last frame taken, for frames without one */
  guint64 handled_pts; /* highest timestamp shown or dropped since start */
  gint64 intervals[AML_PACER_VSYNC_SAMPLES];
  guint n_intervals;
  gint64 last_vsync;
  guint64 shown, repeated, dropped; /* since new */
  void (*shown_callback)(gpointer);
  gpointer shown_data;
};

/* Running time of a frame in us; negative before the segment. */
static gint64 frame_running_us(AmlPresenter *p, guint64 pts_us)
{
  GstClockTime stream, running;
  gint sign;

  if (pts_us > 0)
    stream = (pts_us - 1) * GST_USECOND;
  else
    stream = GST_CLOCK_TIME_IS_VALID(p->last_stream) ? p->last_stream : p->segment.start;
  p->last_stream = stream;
  sign = gst_segment_to_running_time_full(&p->segment, GST_FORMAT_TIME, stream, &running);
  if (sign > 0)
    return (gint64)(running / GST_USECOND);
  if (sign < 0)
    return -(gint64)((running + GST_USECOND - 1) / GST_USECOND);
  return -1;
}

static void hand_back(AmlPresenter *p, const HeldFrame *frame, gboolean drop)
{
  if (p->ops->queue(p->ctx, frame->index, drop) < 0)
    GST_WARNING("cannot hand back buffer %u", frame->index);
  p->handled_pts = MAX(p->handled_pts, frame->pts_us);
}

/* Shows the frame due (or the preroll picture), drops late ones. TRUE on the first
 * frame shown since start. */
static gboolean present(AmlPresenter *p, gboolean vsync, gint64 running)
{
  AmlFrame frames[AML_PACER_MAX_FRAMES];
  AmlPick pick = {.show = -1};
  gboolean first = FALSE, pacing = p->playing && running >= 0;
  guint kept = 0;

  for (guint i = 0; i < p->n_held; i++)
    frames[i] = (AmlFrame){p->held[i].index, p->held[i].running_us};

  if (pacing && p->n_held) {
    gint64 vsync = aml_pacer_vsync_us(p->intervals, p->n_intervals);

    pick = aml_pacer_pick(frames, p->n_held, running + vsync, vsync);
  } else if (!p->shown_since_start) {
    for (guint i = 0; i < p->n_held && pick.show < 0; i++) {
      if (frames[i].running_us < 0)
        pick.drop[pick.n_drop++] = frames[i].index;
      else
        pick.show = (gint)frames[i].index;
    }
  }

  for (guint i = 0; i < p->n_held; i++) {
    gboolean drop = FALSE;

    for (guint d = 0; d < pick.n_drop; d++)
      drop |= p->held[i].index == pick.drop[d];
    if (drop)
      hand_back(p, &p->held[i], TRUE);
    else if ((gint)p->held[i].index == pick.show)
      hand_back(p, &p->held[i], FALSE);
    else
      p->held[kept++] = p->held[i];
  }
  p->n_held = kept;
  /* While playing, a vsync without a new frame repeats the one on screen. */
  if (pick.show < 0 && vsync && pacing && p->shown_since_start)
    p->repeated++;
  if (pick.show >= 0)
    p->shown++;
  p->dropped += pick.n_drop;
  if (pick.show >= 0 && !p->shown_since_start)
    first = p->shown_since_start = TRUE;
  if (pick.show >= 0 || pick.n_drop)
    g_cond_broadcast(&p->handled_cond);
  return first;
}

static void record_vsync(AmlPresenter *p, gboolean vsync, gint64 now)
{
  if (!vsync) {
    p->last_vsync = 0; /* the next interval would span a gap */
    return;
  }
  if (p->last_vsync) {
    if (p->n_intervals == AML_PACER_VSYNC_SAMPLES) {
      memmove(p->intervals, p->intervals + 1, sizeof(p->intervals) - sizeof(p->intervals[0]));
      p->n_intervals--;
    }
    p->intervals[p->n_intervals++] = now - p->last_vsync;
  }
  p->last_vsync = now;
}

static gpointer run(gpointer data)
{
  AmlPresenter *p = data;

  while (!g_atomic_int_get(&p->stopping)) {
    int ret = p->ops->poll_vsync(p->ctx, POLL_TIMEOUT_MS);
    gint64 now = p->ops->now_us(p->ctx), running = p->ops->running_us(p->ctx);
    gboolean first;
    guint index;
    guint64 pts;

    if (g_atomic_int_get(&p->stopping))
      break;
    if (ret < 0) {
      GST_WARNING("vsync poll failed: %s", g_strerror(-ret));
      g_usleep(POLL_TIMEOUT_MS * 1000); /* never spin on a broken device */
    }

    g_mutex_lock(&p->lock);
    record_vsync(p, ret > 0, now);
    while (p->n_held < AML_PACER_MAX_FRAMES && p->ops->dequeue(p->ctx, &index, &pts) == 0)
      p->held[p->n_held++] = (HeldFrame){index, pts, frame_running_us(p, pts)};
    first = present(p, ret > 0, running);
    g_mutex_unlock(&p->lock);

    if (first && p->shown_callback)
      p->shown_callback(p->shown_data);
  }
  return NULL;
}

AmlPresenter *aml_presenter_new(const AmlPresenterOps *ops, void *ctx)
{
  static gsize debug_init = 0;
  AmlPresenter *p = g_new0(AmlPresenter, 1);

  if (g_once_init_enter(&debug_init)) {
    GST_DEBUG_CATEGORY_INIT(aml_presenter_debug, "amlpresenter", 0, "Amlogic frame presenter");
    g_once_init_leave(&debug_init, 1);
  }
  p->ops = ops;
  p->ctx = ctx;
  g_mutex_init(&p->lock);
  g_cond_init(&p->handled_cond);
  gst_segment_init(&p->segment, GST_FORMAT_TIME);
  return p;
}

void aml_presenter_set_segment(AmlPresenter *p, const GstSegment *segment)
{
  g_mutex_lock(&p->lock);
  gst_segment_copy_into(segment, &p->segment);
  g_mutex_unlock(&p->lock);
}

void aml_presenter_start(AmlPresenter *p)
{
  if (p->thread)
    return;
  g_mutex_lock(&p->lock);
  p->running = TRUE;
  p->shown_since_start = FALSE;
  p->last_stream = GST_CLOCK_TIME_NONE;
  p->last_vsync = 0;
  g_mutex_unlock(&p->lock);
  g_atomic_int_set(&p->stopping, 0);
  p->thread = g_thread_new("amlpresenter", run, p);
}

void aml_presenter_flush(AmlPresenter *p)
{
  g_mutex_lock(&p->lock);
  p->running = FALSE;
  g_cond_broadcast(&p->handled_cond);
  g_mutex_unlock(&p->lock);
  if (p->thread) {
    g_atomic_int_set(&p->stopping, 1);
    g_thread_join(p->thread);
    p->thread = NULL;
  }
  g_mutex_lock(&p->lock);
  for (guint i = 0; i < p->n_held; i++)
    hand_back(p, &p->held[i], TRUE);
  p->n_held = 0;
  p->handled_pts = 0;
  g_mutex_unlock(&p->lock);
}

void aml_presenter_set_playing(AmlPresenter *p, gboolean playing)
{
  g_mutex_lock(&p->lock);
  p->playing = playing;
  g_mutex_unlock(&p->lock);
}

guint64 aml_presenter_wait_shown(AmlPresenter *p, guint64 pts_us, gint64 timeout_us)
{
  gint64 deadline = g_get_monotonic_time() + timeout_us;
  guint64 handled;

  g_mutex_lock(&p->lock);
  while (p->running && p->handled_pts < pts_us && g_cond_wait_until(&p->handled_cond, &p->lock, deadline))
    ;
  handled = p->handled_pts;
  g_mutex_unlock(&p->lock);
  return handled;
}

void aml_presenter_get_stats(AmlPresenter *p, guint64 *shown, guint64 *repeated, guint64 *dropped)
{
  g_mutex_lock(&p->lock);
  *shown = p->shown;
  *repeated = p->repeated;
  *dropped = p->dropped;
  g_mutex_unlock(&p->lock);
}

void aml_presenter_set_shown_callback(AmlPresenter *p, void (*callback)(gpointer), gpointer data)
{
  g_mutex_lock(&p->lock);
  p->shown_callback = callback;
  p->shown_data = data;
  g_mutex_unlock(&p->lock);
}

void aml_presenter_free(AmlPresenter *p)
{
  aml_presenter_flush(p);
  g_mutex_clear(&p->lock);
  g_cond_clear(&p->handled_cond);
  g_free(p);
}
