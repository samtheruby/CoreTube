/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host test for gstamlvideosink.c: vcodec_profile parsing, AV1 framing, decoder config,
 * and the sysfs effects of opening/closing the decoder (on a fake sysfs tree). */
#define SYSFS_ROOT "/tmp/fakesys"
#define VIDEO_HOLD_TIMEOUT (200 * GST_MSECOND)
#include "gstamlvideosink.c"
#include "aml_pacer.c"
#include "aml_presenter.c"
#include <assert.h>

static GByteArray *written;
static int fake_write(codec_para_t *c, void *buf, int len) { (void)c; g_byte_array_append(written, buf, len); return len; }

static int init_result, init_video_path = -1;
static guint64 checkins[4]; /* timestamps queued since the test cleared checkin_count */
static guint checkin_count;
static int fake_init(codec_para_t *c) { init_video_path = c->video_path; return init_result; }
static int fake_ok(codec_para_t *c) { (void)c; return 0; }
static int fake_checkin(codec_para_t *c, guint64 pts)
{
  (void)c;
  if (checkin_count < G_N_ELEMENTS(checkins))
    checkins[checkin_count] = pts;
  checkin_count++;
  return 0;
}
static int fake_set_int(codec_para_t *c, int v) { (void)c; (void)v; return 0; }

/* amlvideo for the sink tests: free-running vsyncs, no pipeline clock (so only the
 * preroll picture shows), and the frames the test says the decoder put out. */
static GMutex sink_video_lock;
static guint64 sink_video_pts[8];
static guint sink_video_ready, sink_video_index;
static int sink_video_poll(void *ctx, int timeout_ms) { (void)ctx; (void)timeout_ms; g_usleep(2000); return 1; }
static int sink_video_dequeue(void *ctx, guint *index, guint64 *pts_us)
{
  int ret = -EAGAIN;

  (void)ctx;
  g_mutex_lock(&sink_video_lock);
  if (sink_video_ready) {
    *pts_us = sink_video_pts[0];
    *index = sink_video_index++;
    memmove(sink_video_pts, sink_video_pts + 1, sizeof(sink_video_pts) - sizeof(sink_video_pts[0]));
    sink_video_ready--;
    ret = 0;
  }
  g_mutex_unlock(&sink_video_lock);
  return ret;
}
static int sink_video_queue(void *ctx, guint index, gboolean drop) { (void)ctx; (void)index; (void)drop; return 0; }
static gint64 sink_video_running(void *ctx) { (void)ctx; return -1; }
static gint64 sink_video_now(void *ctx) { (void)ctx; return g_get_monotonic_time(); }
static const AmlPresenterOps sink_video_ops = {sink_video_poll, sink_video_dequeue, sink_video_queue,
                                               sink_video_running, sink_video_now};

static void sink_video_decoded(guint64 pts_us)
{
  g_mutex_lock(&sink_video_lock);
  sink_video_pts[sink_video_ready++] = pts_us;
  g_mutex_unlock(&sink_video_lock);
}

static void put(const char *path, const char *value)
{
  gchar *full = g_strconcat(SYSFS_ROOT, path, NULL), *dir = g_path_get_dirname(full);
  g_mkdir_with_parents(dir, 0755);
  assert(g_file_set_contents(full, value, -1, NULL));
  g_free(dir);
  g_free(full);
}

static gboolean is(const char *path, const char *want)
{
  gchar *full = g_strconcat(SYSFS_ROOT, path, NULL), *got = NULL;
  gboolean same = g_file_get_contents(full, &got, NULL, NULL) && strcmp(g_strstrip(got), want) == 0;
  if (!same)
    g_printerr("%s is \"%s\", want \"%s\"\n", path, got ? got : "(missing)", want);
  g_free(got);
  g_free(full);
  return same;
}

/* is(), waiting up to a second for another thread to write the value. */
static gboolean becomes(const char *path, const char *want)
{
  gchar *full = g_strconcat(SYSFS_ROOT, path, NULL), *got = NULL;
  gboolean same = FALSE;

  for (int i = 0; i < 100 && !same; i++, g_usleep(10000)) {
    g_free(got);
    got = NULL;
    same = g_file_get_contents(full, &got, NULL, NULL) && strcmp(g_strstrip(got), want) == 0;
  }
  g_free(got);
  g_free(full);
  return same || is(path, want);
}

static void render_at(GstAmlVideoSink *sink, GstClockTime pts)
{
  GstBuffer *buf = gst_buffer_new_allocate(NULL, 4, NULL);
  GST_BUFFER_PTS(buf) = pts;
  assert(gst_aml_video_sink_render(GST_BASE_SINK(sink), buf) == GST_FLOW_OK);
  gst_buffer_unref(buf);
}

#define BLACKOUT "/sys/class/video/blackout_policy"
#define MUTE "/sys/class/amhdmitx/amhdmitx0/vid_mute"
#define DISABLE "/sys/class/video/disable_video"
#define TSYNC "/sys/class/tsync/enable"
#define FREERUN "/sys/class/video/freerun_mode"

/* Opening and closing the decoder: blackout_policy and the screen mute. */
static void test_decoder_sysfs(void)
{
  GstCaps *h264 = gst_caps_from_string("video/x-h264, width=1920, height=1080");
  GstAmlVideoSink *sink;

  put(BLACKOUT, "0\n");
  put(MUTE, "0\n");
  put(DISABLE, "0\n");
  put("/sys/class/graphics/fb0/free_scale_axis", "0 0 1919 1079\n");
  put("/sys/class/graphics/fb0/window_axis", "0 0 3839 2159\n");

  sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, NULL);
  gst_segment_init(&GST_BASE_SINK(sink)->segment, GST_FORMAT_TIME);

  /* Fullscreen video: the kernel must not keep the last frame, and the screen is
   * muted until the first decoded frame is shown. */
  g_object_set(sink, "rectangle", "0,0,1920,1080", NULL);
  assert(open_decoder(sink, h264));
  assert(is(BLACKOUT, "1") && is(MUTE, "1") && is(DISABLE, "2"));
  render_at(sink, GST_SECOND);
  g_usleep(50 * 1000);
  assert(is(MUTE, "1"));                  /* nothing decoded yet */
  sink_video_decoded(1000001);
  assert(becomes(MUTE, "0"));             /* first frame shown */

  /* A seek mutes again until a frame decoded after it shows. */
  gst_aml_video_sink_event(GST_BASE_SINK(sink), gst_event_new_flush_stop(TRUE));
  assert(is(MUTE, "1"));
  render_at(sink, GST_SECOND);
  g_usleep(50 * 1000);
  assert(is(MUTE, "1"));
  g_usleep(400 * 1000);                   /* the timeout unmutes */
  assert(is(MUTE, "0"));

  /* Closing restores blackout_policy and leaves the screen unmuted. */
  gst_aml_video_sink_event(GST_BASE_SINK(sink), gst_event_new_flush_stop(TRUE));
  assert(is(MUTE, "1"));
  close_decoder(sink);
  assert(is(BLACKOUT, "0") && is(MUTE, "0") && is(DISABLE, "1"));

  /* A video in a window (home screen preview) never mutes the screen. */
  g_object_set(sink, "rectangle", "100,100,640,360", NULL);
  assert(open_decoder(sink, h264));
  assert(is(BLACKOUT, "1") && is(MUTE, "0"));
  close_decoder(sink);
  assert(is(BLACKOUT, "0"));

  /* A decoder that fails to open leaves blackout_policy as it was. */
  init_result = -1;
  assert(!open_decoder(sink, h264));
  assert(is(BLACKOUT, "0") && is(MUTE, "0"));
  init_result = 0;

  gst_object_unref(sink);
  gst_caps_unref(h264);
}

/* The decoder puts frames out on amlvideo, the hardware clock (tsync) is off, and the
 * video layer shows each frame as soon as it is queued (freerun); closing restores both. */
static void test_open_uses_amlvideo_path(void)
{
  GstCaps *h264 = gst_caps_from_string("video/x-h264, width=1920, height=1080");
  GstAmlVideoSink *sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, NULL);

  put(TSYNC, "1\n");
  put(FREERUN, "0\n");
  g_object_set(sink, "rectangle", "100,100,640,360", NULL);
  assert(open_decoder(sink, h264));
  assert(init_video_path == FRAME_BASE_PATH_AMLVIDEO_AMVIDEO);
  assert(is(TSYNC, "0") && is(FREERUN, "1"));
  close_decoder(sink);
  assert(is(TSYNC, "1") && is(FREERUN, "0"));

  /* Without amlvideo there is no way to show frames: the open fails. */
  assert(remove(SYSFS_ROOT "/dev/video10") == 0);
  assert(!open_decoder(sink, h264));
  put("/dev/video10", "");

  gst_object_unref(sink);
  gst_caps_unref(h264);
}

/* Timestamps go to the decoder as the frame's own time in us (+1, as 0 means none);
 * the presenter maps them to running time with the segment. */
static void test_checkin_is_stream_time_us(void)
{
  GstCaps *h264 = gst_caps_from_string("video/x-h264, width=1920, height=1080");
  GstAmlVideoSink *sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, NULL);
  GstSegment *segment = &GST_BASE_SINK(sink)->segment;

  gst_segment_init(segment, GST_FORMAT_TIME);
  g_object_set(sink, "rectangle", "100,100,640,360", NULL);
  assert(open_decoder(sink, h264));
  checkin_count = 0;
  render_at(sink, 2500 * GST_MSECOND);
  segment->start = segment->time = 5 * GST_SECOND;
  render_at(sink, 7 * GST_SECOND);
  assert(checkin_count == 2 && checkins[0] == 2500001 && checkins[1] == 7000001);
  close_decoder(sink);
  gst_object_unref(sink);
  gst_caps_unref(h264);
}

/* The hardware clock stays off through playing, a seek and a pause. */
static void test_hardware_clock_stays_off(void)
{
  GstElement *sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, "async", FALSE, NULL);
  GstPad *pad = gst_element_get_static_pad(sink, "sink");
  GstCaps *h264 = gst_caps_from_string(H264_CAPS ", width=1920, height=1080");
  GstSegment segment;
  GstBuffer *buf = gst_buffer_new_wrapped(g_memdup2("\0\0\0\1", 4), 4);

  put(TSYNC, "1\n");
  g_object_set(sink, "rectangle", "100,100,640,360", NULL);
  assert(gst_element_set_state(sink, GST_STATE_PLAYING) == GST_STATE_CHANGE_SUCCESS);
  assert(gst_pad_send_event(pad, gst_event_new_stream_start("clock")));
  assert(gst_pad_send_event(pad, gst_event_new_caps(h264)));
  gst_segment_init(&segment, GST_FORMAT_TIME);
  assert(gst_pad_send_event(pad, gst_event_new_segment(&segment)));
  GST_BUFFER_PTS(buf) = 0;
  assert(gst_pad_chain(pad, buf) == GST_FLOW_OK);
  assert(is(TSYNC, "0"));
  assert(gst_pad_send_event(pad, gst_event_new_flush_start()));
  assert(gst_pad_send_event(pad, gst_event_new_flush_stop(TRUE)));
  assert(is(TSYNC, "0"));
  gst_element_set_state(sink, GST_STATE_PAUSED);
  gst_element_set_state(sink, GST_STATE_PLAYING);
  assert(is(TSYNC, "0"));
  gst_element_set_state(sink, GST_STATE_NULL);
  assert(is(TSYNC, "1"));
  gst_object_unref(pad);
  gst_object_unref(sink);
  gst_caps_unref(h264);
}

/* An accurate seek starts the segment at the target, but upstream sends frames from
 * the keyframe before it, marked as Cobalt's RDK player marks them (decode only, 1/60 s
 * long). The decoder needs them all, timed at the segment start. */
static void test_seek_inside_gop(void)
{
  GstElement *sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, "async", FALSE, NULL);
  GstPad *pad = gst_element_get_static_pad(sink, "sink");
  GstCaps *h264 = gst_caps_from_string(H264_CAPS ", width=1920, height=1080");
  GstSegment segment;

  g_object_set(sink, "rectangle", "100,100,640,360", NULL);
  assert(gst_element_set_state(sink, GST_STATE_PLAYING) == GST_STATE_CHANGE_SUCCESS);
  assert(gst_pad_send_event(pad, gst_event_new_stream_start("seek")));
  assert(gst_pad_send_event(pad, gst_event_new_caps(h264)));
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = segment.time = segment.position = 5 * GST_SECOND;
  assert(gst_pad_send_event(pad, gst_event_new_segment(&segment)));

  g_byte_array_set_size(written, 0);
  checkin_count = 0;
  for (GstClockTime pts = 4267 * GST_MSECOND; pts <= 5 * GST_SECOND; pts += 733 * GST_MSECOND) {
    GstBuffer *buf = gst_buffer_new_wrapped(g_memdup2("\0\0\0\1", 4), 4);

    GST_BUFFER_PTS(buf) = pts;
    GST_BUFFER_DURATION(buf) = GST_SECOND / 60;
    if (pts < segment.start)
      GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DECODE_ONLY);
    assert(gst_pad_chain(pad, buf) == GST_FLOW_OK);
  }
  assert(written->len == 8);               /* the keyframe at 4.267 s reached the decoder */
  assert(checkin_count == 2);              /* its own time: the presenter drops it, it never shows */
  assert(checkins[0] == 4267001 && checkins[1] == 5000001);

  gst_element_set_state(sink, GST_STATE_NULL);
  gst_object_unref(pad);
  gst_object_unref(sink);
  gst_caps_unref(h264);
}

static gpointer push_buffers(gpointer data)
{
  GstPad *pad = data;
  GstClockTime *pts = g_object_get_data(G_OBJECT(pad), "pts");

  for (; *pts != GST_CLOCK_TIME_NONE; pts++) {
    GstBuffer *buf = gst_buffer_new_wrapped(g_memdup2("\0\0\0\1", 4), 4);

    GST_BUFFER_PTS(buf) = *pts;
    GST_BUFFER_DURATION(buf) = GST_SECOND / 60;
    if (gst_pad_chain(pad, buf) != GST_FLOW_OK)
      break;
  }
  return NULL;
}

/* While a seek prerolls, Cobalt's player waits for frames past the target to leave
 * its queue. The frames before the target must go to the decoder without the sink
 * prerolling on them; the first frame at the target prerolls as usual. */
static void test_preroll_after_seek(void)
{
  GstElement *sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, NULL);
  GstPad *pad = gst_element_get_static_pad(sink, "sink");
  GstCaps *h264 = gst_caps_from_string(H264_CAPS ", width=1920, height=1080");
  GstClockTime before[] = {4267 * GST_MSECOND, 4300 * GST_MSECOND, GST_CLOCK_TIME_NONE};
  GstClockTime target[] = {5 * GST_SECOND, GST_CLOCK_TIME_NONE};
  GstSegment segment;
  GThread *thread;

  g_object_set(sink, "rectangle", "100,100,640,360", NULL);
  assert(gst_element_set_state(sink, GST_STATE_PAUSED) == GST_STATE_CHANGE_ASYNC);
  assert(gst_pad_send_event(pad, gst_event_new_stream_start("seek")));
  assert(gst_pad_send_event(pad, gst_event_new_caps(h264)));
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = segment.time = segment.position = 5 * GST_SECOND;
  assert(gst_pad_send_event(pad, gst_event_new_segment(&segment)));

  g_byte_array_set_size(written, 0);
  checkin_count = 0;
  g_object_set_data(G_OBJECT(pad), "pts", before);
  thread = g_thread_new("before", push_buffers, pad);
  g_usleep(300 * 1000);
  assert(written->len == 8);               /* both reached the decoder without blocking */
  assert(checkin_count == 2 && checkins[0] == 4267001 && checkins[1] == 4300001);
  assert(gst_element_get_state(sink, NULL, NULL, 0) == GST_STATE_CHANGE_ASYNC); /* no preroll yet */
  gst_element_set_state(sink, GST_STATE_READY); /* unblocks the thread if it hangs */
  g_thread_join(thread);

  assert(gst_element_set_state(sink, GST_STATE_PAUSED) == GST_STATE_CHANGE_ASYNC);
  assert(gst_pad_send_event(pad, gst_event_new_stream_start("seek")));
  assert(gst_pad_send_event(pad, gst_event_new_caps(h264)));
  assert(gst_pad_send_event(pad, gst_event_new_segment(&segment)));
  g_object_set_data(G_OBJECT(pad), "pts", target);
  g_byte_array_set_size(written, 0);
  thread = g_thread_new("target", push_buffers, pad);
  assert(gst_element_get_state(sink, NULL, NULL, GST_SECOND) == GST_STATE_CHANGE_SUCCESS);
  assert(written->len == 4);               /* paused, the target frame is decoded for the preroll picture */
  gst_element_set_state(sink, GST_STATE_PLAYING);
  g_usleep(100 * 1000);
  assert(written->len == 4);               /* and not sent again when playing starts */

  gst_element_set_state(sink, GST_STATE_NULL);
  g_thread_join(thread);
  gst_object_unref(pad);
  gst_object_unref(sink);
  gst_caps_unref(h264);
}

/* Plays frames every frame_us on a display with a vsync every vsync_us. A frame queued
 * at vsync k appears at running time k * vsync_us. Counts how many vsyncs each frame
 * stays on screen, the frames dropped, and the vsyncs that showed nothing new. */
typedef struct { guint on_screen[4096]; guint drops, repeats, last_shown; } Paced;

static void pace(Paced *r, gint64 frame_us, gint64 vsync_us, guint vsyncs)
{
  AmlFrame queue[16];
  guint n = 0, next = 0;
  gint current = -1;

  memset(r, 0, sizeof(*r));
  for (guint k = 0; k < vsyncs; k++) {
    AmlPick pick;
    guint kept = 0;

    for (; n < G_N_ELEMENTS(queue); n++, next++)
      queue[n] = (AmlFrame){next, (gint64)next * frame_us};
    pick = aml_pacer_pick(queue, n, (gint64)k * vsync_us, vsync_us);
    r->drops += pick.n_drop;
    if (pick.show >= 0)
      current = pick.show;
    else if (current >= 0)
      r->repeats++;
    if (current >= 0) {
      r->on_screen[current]++;
      r->last_shown = (guint)current;
    }
    for (guint i = 0; i < n; i++) {
      gboolean gone = (gint)queue[i].index == pick.show;

      for (guint d = 0; d < pick.n_drop; d++)
        gone |= queue[i].index == pick.drop[d];
      if (!gone)
        queue[kept++] = queue[i];
    }
    n = kept;
  }
}

/* 23.976 fps on 60 Hz: 3:2, each frame on screen 2 or 3 vsyncs in turn. The 0.1%
 * rate difference may break the alternation once in 10 s; never a drop. */
static void test_cadence_24_on_60(void)
{
  Paced r;
  guint breaks = 0;

  pace(&r, 41708, 16667, 600);
  assert(r.drops == 0 && r.last_shown >= 238);
  for (guint i = 0; i < r.last_shown; i++) {
    assert(r.on_screen[i] == 2 || r.on_screen[i] == 3);
    if (i > 0 && r.on_screen[i] == r.on_screen[i - 1])
      breaks++;
  }
  assert(breaks <= 1);
}

/* 23.976 fps on 24 Hz: the frame on screen repeats once every ~1000 vsyncs. */
static void test_cadence_23976_on_24(void)
{
  Paced r;

  pace(&r, 41708, 41667, 3000);
  assert(r.drops == 0);
  assert(r.repeats == 3);
}

static void test_late_frames_dropped(void)
{
  AmlFrame queue[] = {{0, 0}, {1, 16667}, {2, 33334}};
  AmlPick pick = aml_pacer_pick(queue, 3, 50000, 16667);

  assert(pick.show == 2);
  assert(pick.n_drop == 2 && pick.drop[0] == 0 && pick.drop[1] == 1);
}

static void test_before_segment_dropped(void)
{
  AmlFrame queue[] = {{7, -40000}, {8, -20000}, {9, 0}};
  AmlPick pick = aml_pacer_pick(queue, 3, 0, 16667);

  assert(pick.show == 9);
  assert(pick.n_drop == 2 && pick.drop[0] == 7 && pick.drop[1] == 8);
}

/* Nothing due yet: show nothing, drop nothing. */
static void test_nothing_due(void)
{
  AmlFrame queue[] = {{0, 100000}};
  AmlPick pick = aml_pacer_pick(queue, 1, 50000, 16667);

  assert(pick.show == -1 && pick.n_drop == 0);
}

/* 30 fps at 2x speed on 60 Hz: running times every 16667 us, each frame shown once. */
static void test_double_speed_shows_each_frame_once(void)
{
  Paced r;

  pace(&r, 16667, 16667, 600);
  assert(r.drops == 0 && r.repeats == 0);
  for (guint i = 0; i < 600; i++)
    assert(r.on_screen[i] == 1);
}

static void test_vsync_median(void)
{
  const gint64 intervals[] = {16600, 16700, 16650, 99999, 16680, 16640, 16660, 16670};

  assert(aml_pacer_vsync_us(intervals, 8) == 16665);
  assert(aml_pacer_vsync_us(intervals, 3) == 16667);
}

/* Fake amlvideo for the presenter: the test releases each poll (a vsync or a timeout),
 * decides which frames the decoder has ready, and reads back every frame queued. */
typedef struct {
  GMutex lock;
  GCond cond;
  AmlPresenter *presenter;
  guint ticks;          /* polls the test has released and the presenter not yet taken */
  gboolean vsync;       /* released polls are vsyncs (else timeouts) */
  gboolean in_poll;     /* the presenter waits in poll */
  guint polls, bad_timeouts;
  gint64 now, base;     /* fake clock; running time = now - base while the clock runs */
  gboolean clock_running;
  struct { guint index; guint64 pts; } decoded[512];
  guint n_decoded, next_decoded;
  struct { guint index; gboolean drop; } log[1024];
  guint n_log;
} FakeVideo;

static int fake_poll_vsync(void *ctx, int timeout_ms)
{
  FakeVideo *f = ctx;
  int ret = 0;

  g_mutex_lock(&f->lock);
  if (timeout_ms != 50)
    f->bad_timeouts++;
  f->in_poll = TRUE;
  g_cond_broadcast(&f->cond);
  while (!f->ticks && !g_atomic_int_get(&f->presenter->stopping))
    g_cond_wait_until(&f->cond, &f->lock, g_get_monotonic_time() + 5000);
  f->in_poll = FALSE;
  if (f->ticks) {
    f->ticks--;
    f->polls++;
    if (f->vsync) {
      f->now += 16667;
      ret = 1;
    }
  }
  g_mutex_unlock(&f->lock);
  return ret;
}

static int fake_dequeue(void *ctx, guint *index, guint64 *pts_us)
{
  FakeVideo *f = ctx;
  int ret = -EAGAIN;

  g_mutex_lock(&f->lock);
  if (f->next_decoded < f->n_decoded) {
    *index = f->decoded[f->next_decoded].index;
    *pts_us = f->decoded[f->next_decoded].pts;
    f->next_decoded++;
    ret = 0;
  }
  g_mutex_unlock(&f->lock);
  return ret;
}

static int fake_queue(void *ctx, guint index, gboolean drop)
{
  FakeVideo *f = ctx;

  g_mutex_lock(&f->lock);
  assert(f->n_log < G_N_ELEMENTS(f->log));
  f->log[f->n_log].index = index;
  f->log[f->n_log].drop = drop;
  f->n_log++;
  g_mutex_unlock(&f->lock);
  return 0;
}

static gint64 fake_running_us(void *ctx)
{
  FakeVideo *f = ctx;
  gint64 running;

  g_mutex_lock(&f->lock);
  running = f->clock_running ? f->now - f->base : -1;
  g_mutex_unlock(&f->lock);
  return running;
}

static gint64 fake_now_us(void *ctx)
{
  FakeVideo *f = ctx;
  gint64 now;

  g_mutex_lock(&f->lock);
  now = f->now;
  g_mutex_unlock(&f->lock);
  return now;
}

static const AmlPresenterOps fake_video_ops = {fake_poll_vsync, fake_dequeue, fake_queue, fake_running_us, fake_now_us};

static FakeVideo *fake_video_new(void)
{
  FakeVideo *f = g_new0(FakeVideo, 1);
  GstSegment segment;

  g_mutex_init(&f->lock);
  g_cond_init(&f->cond);
  f->vsync = TRUE;
  f->presenter = aml_presenter_new(&fake_video_ops, f);
  gst_segment_init(&segment, GST_FORMAT_TIME);
  aml_presenter_set_segment(f->presenter, &segment);
  return f;
}

static void fake_video_free(FakeVideo *f)
{
  aml_presenter_free(f->presenter);
  g_mutex_clear(&f->lock);
  g_cond_clear(&f->cond);
  g_free(f);
}

/* The decoder has a frame ready; pts as amlvideo gives it (stream time in us + 1, 0 = none). */
static void decoded(FakeVideo *f, guint index, guint64 pts)
{
  g_mutex_lock(&f->lock);
  f->decoded[f->n_decoded].index = index;
  f->decoded[f->n_decoded].pts = pts;
  f->n_decoded++;
  g_mutex_unlock(&f->lock);
}

/* Releases n polls and waits until the presenter has handled each one. */
static void tick(FakeVideo *f, guint n, gboolean vsync)
{
  g_mutex_lock(&f->lock);
  for (guint i = 0; i < n; i++) {
    while (!f->in_poll)
      g_cond_wait(&f->cond, &f->lock);
    f->vsync = vsync;
    f->ticks = 1;
    g_cond_broadcast(&f->cond);
    while (f->ticks || !f->in_poll)
      g_cond_wait(&f->cond, &f->lock);
  }
  g_mutex_unlock(&f->lock);
}

static void play(FakeVideo *f, gint64 running_now)
{
  g_mutex_lock(&f->lock);
  f->clock_running = TRUE;
  f->base = f->now - running_now;
  g_mutex_unlock(&f->lock);
  aml_presenter_set_playing(f->presenter, TRUE);
}

static guint count_queued(FakeVideo *f, guint index, gboolean drop)
{
  guint n = 0;

  for (guint i = 0; i < f->n_log; i++)
    n += f->log[i].index == index && f->log[i].drop == drop;
  return n;
}

static void test_presents_in_order(void)
{
  FakeVideo *f = fake_video_new();

  for (guint i = 0; i < 10; i++)
    decoded(f, i, 50000 + i * 33333 + 1);
  play(f, 0);
  aml_presenter_start(f->presenter);
  tick(f, 60, TRUE);
  assert(f->n_log == 10);
  for (guint i = 0; i < 10; i++)
    assert(f->log[i].index == i && !f->log[i].drop);
  fake_video_free(f);
}

/* Paused, the first frame is the preroll picture: shown once, the others wait. */
static void test_paused_shows_first_frame_once(void)
{
  FakeVideo *f = fake_video_new();

  for (guint i = 0; i < 3; i++)
    decoded(f, i, i * 33333 + 1);
  aml_presenter_start(f->presenter);
  tick(f, 5, TRUE);
  assert(f->n_log == 1 && f->log[0].index == 0 && !f->log[0].drop);

  play(f, 0);
  tick(f, 6, TRUE);
  assert(f->n_log == 3 && count_queued(f, 1, FALSE) == 1 && count_queued(f, 2, FALSE) == 1);
  fake_video_free(f);
}

/* A flush hands every held buffer back, so the decoder can be reset or closed. */
static void test_flush_returns_every_buffer(void)
{
  FakeVideo *f = fake_video_new();

  for (guint i = 0; i < 4; i++)
    decoded(f, i, i * 33333 + 1);
  aml_presenter_start(f->presenter);
  tick(f, 1, TRUE);
  aml_presenter_flush(f->presenter);
  assert(f->n_log == 4);
  assert(count_queued(f, 0, FALSE) == 1);
  for (guint i = 1; i < 4; i++)
    assert(count_queued(f, i, TRUE) == 1);
  fake_video_free(f);
}

/* No new frames: the screen keeps its frame, and every wait is a 50 ms poll. */
static void test_stall_keeps_last_frame(void)
{
  FakeVideo *f = fake_video_new();

  decoded(f, 0, 1);
  play(f, 0);
  aml_presenter_start(f->presenter);
  tick(f, 1, TRUE);
  assert(f->n_log == 1);
  tick(f, 20, FALSE);
  assert(f->n_log == 1);
  assert(f->polls == 21 && f->bad_timeouts == 0);
  fake_video_free(f);
}

static void count_shown(gpointer data) { (*(guint *)data)++; }

static void test_shown_callback_once_per_start(void)
{
  FakeVideo *f = fake_video_new();
  guint calls = 0;

  aml_presenter_set_shown_callback(f->presenter, count_shown, &calls);
  for (guint i = 0; i < 3; i++)
    decoded(f, i, 50000 + i * 33333 + 1);
  play(f, 0);
  aml_presenter_start(f->presenter);
  tick(f, 10, TRUE);
  assert(f->n_log == 3 && calls == 1);

  aml_presenter_flush(f->presenter);
  decoded(f, 3, 1);
  play(f, 0);
  aml_presenter_start(f->presenter);
  tick(f, 2, TRUE);
  assert(count_queued(f, 3, FALSE) == 1 && calls == 2);
  fake_video_free(f);
}

/* amlvideo gives the first frame after the decoder opens no timestamp (pts 0). It
 * takes the segment start, so it is shown, not dropped as before the segment. */
static void test_frame_without_pts_is_shown(void)
{
  FakeVideo *f = fake_video_new();
  GstSegment segment;

  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = segment.time = 5 * GST_SECOND;
  aml_presenter_set_segment(f->presenter, &segment);
  decoded(f, 0, 0);
  decoded(f, 1, 5033334);
  aml_presenter_start(f->presenter);
  tick(f, 1, TRUE);
  assert(f->n_log == 1 && f->log[0].index == 0 && !f->log[0].drop);
  fake_video_free(f);
}

/* 23.976 fps on 60 Hz through the presenter: per vsync a new frame or a repeat. */
static void test_stats_count_shown_repeated_dropped(void)
{
  FakeVideo *f = fake_video_new();
  guint64 shown, repeated, dropped;

  for (guint i = 0; i < 260; i++)
    decoded(f, i, (guint64)i * 41708 + 1);
  play(f, -16667); /* the first vsync is at running time 0 */
  aml_presenter_start(f->presenter);
  tick(f, 600, TRUE);
  aml_presenter_get_stats(f->presenter, &shown, &repeated, &dropped);
  assert(shown == 240 && repeated == 360 && dropped == 0);
  fake_video_free(f);
}

/* End of stream waits for the last frame on screen, or gives up at the timeout. */
static void test_wait_shown(void)
{
  FakeVideo *f = fake_video_new();

  decoded(f, 0, 50001);
  decoded(f, 1, 83334);
  play(f, 0);
  aml_presenter_start(f->presenter);
  tick(f, 6, TRUE);
  assert(aml_presenter_wait_shown(f->presenter, 83334, G_USEC_PER_SEC) == 83334);
  assert(aml_presenter_wait_shown(f->presenter, 116667, 20000) == 83334);
  fake_video_free(f);
}

/* A last frame that is dropped (here: before the segment) also ends the wait, with its
 * timestamp, so end of stream does not wait out the timeout. */
static void test_wait_shown_counts_dropped_frames(void)
{
  FakeVideo *f = fake_video_new();
  GstSegment segment;

  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = segment.time = 10 * GST_SECOND;
  aml_presenter_set_segment(f->presenter, &segment);
  decoded(f, 0, 9000001);
  play(f, 0);
  aml_presenter_start(f->presenter);
  tick(f, 1, TRUE);
  assert(count_queued(f, 0, TRUE) == 1);
  assert(aml_presenter_wait_shown(f->presenter, 9000001, 0) == 9000001);
  fake_video_free(f);
}

/* The S922Z box's vcodec_profile (excerpt), and one with AV1 hardware. */
static const char *g12b = "h264:4k;\nhevc:8k, 8bit, 10bit, dwrite, compressed, frame_dv, fence;\nav1_unsupport:;\nmav1:;\nvp9:8k, 10bit, dwrite, compressed, fence;\n";
static const char *s905x4 = "h264:4k;\nav1:4k, 10bit, dwrite, compressed;\nvp9:4k, 10bit;\n";

int main(void)
{
  gst_init(NULL, NULL);
  put("/sys/class/amstream/vcodec_profile",
      "mh264:4k, compressed;\nhevc_fb:4k, compressed;\nvp9_fb:4k, compressed;\nav1_fb:4k, compressed;\n");
  GstCaps *hardware = supported_caps();
  GstCaps *expected = gst_caps_from_string(H264_CAPS "; " H265_CAPS "; " VP9_CAPS "; " AV1_CAPS);
  assert(gst_caps_is_equal(hardware, expected));
  gst_caps_unref(hardware);
  gst_caps_unref(expected);
  assert(profile_has(g12b, "h264", NULL));
  assert(profile_has(g12b, "hevc", NULL));
  assert(profile_has(g12b, "vp9", "compressed"));
  assert(!profile_has(g12b, "av1", "compressed"));
  assert(!profile_has(g12b, "av1", NULL));      /* "av1_unsupport" and "mav1" are other names */
  assert(profile_has(s905x4, "av1", "compressed"));
  assert(!profile_has(s905x4, "vp9", "compressed")); /* no compressed output: Kodi skips it too */
  assert(!profile_has(s905x4, "hevc", NULL));

  GstAmlVideoSink *sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, NULL);
  amcodec.write = fake_write;
  written = g_byte_array_new();
  /* temporal delimiter (type 2, size 0) + OBU_FRAME (type 6, size 3) */
  const guint8 tu[] = {0x12, 0x00, 0x32, 0x03, 0xaa, 0xbb, 0xcc};
  assert(write_av1(sink, tu, sizeof(tu)) == GST_FLOW_OK);
  const guint8 want[] = {
    0, 0, 0, 10, 0xff, 0xff, 0xff, 0xf5, 0, 0, 0, 1, 'A', 'M', 'L', 'V', 0x86, 0x80, 0x80, 0x00, 0x12, 0x00,
    0, 0, 0, 13, 0xff, 0xff, 0xff, 0xf2, 0, 0, 0, 1, 'A', 'M', 'L', 'V', 0x89, 0x80, 0x80, 0x00, 0x32, 0x03, 0xaa, 0xbb, 0xcc};
  assert(written->len == sizeof(want) && memcmp(written->data, want, sizeof(want)) == 0);

  /* size field past the end: frame dropped, nothing written */
  g_byte_array_set_size(written, 0);
  const guint8 broken[] = {0x32, 0x09, 0xaa};
  assert(write_av1(sink, broken, sizeof(broken)) == GST_FLOW_OK && written->len == 0);

  GstCaps *av1 = gst_caps_from_string("video/x-av1, width=3840, height=2160");
  gchar *c = decoder_config(VFORMAT_AV1, av1, 3840, 2160);
  assert(strcmp(c, "av1_max_pic_w:3840;av1_max_pic_h:2160;") == 0);
  g_free(c);

  GstCaps *sdr = gst_caps_from_string("video/x-vp9");
  assert(decoder_config(VFORMAT_VP9, sdr, 1920, 1080) == NULL);
  GstCaps *hdr = gst_caps_from_string("video/x-vp9, colorimetry=(string)bt2100-pq, "
      "mastering-display-info=(string)35400:14600:8500:39850:6550:2300:15635:16450:10000000:50, "
      "content-light-level=(string)1000:400");
  c = decoder_config(VFORMAT_VP9, hdr, 3840, 2160);
  g_print("%s\n", c);
  assert(strcmp(c, "HDRStaticInfo:1;mR.x:35400;mR.y:14600;mG.x:8500;mG.y:39850;mB.x:6550;mB.y:2300;"
                   "mW.x:15635;mW.y:16450;mMaxDL:10000000;mMinDL:50;mCLLPresent:1;mMaxCLL:1000;mMaxFALL:400;mTransfer:16") == 0);
  amcodec.init = fake_init;
  amcodec.close = amcodec.reset = amcodec.pause = fake_ok;
  amcodec.checkin_pts_us64 = fake_checkin;
  amcodec.set_eos = fake_set_int;
  presenter_ops = &sink_video_ops;
  put("/dev/video10", "");
  test_decoder_sysfs();
  test_open_uses_amlvideo_path();
  test_checkin_is_stream_time_us();
  test_hardware_clock_stays_off();
  test_seek_inside_gop();
  test_preroll_after_seek();
  test_cadence_24_on_60();
  test_cadence_23976_on_24();
  test_late_frames_dropped();
  test_before_segment_dropped();
  test_nothing_due();
  test_double_speed_shows_each_frame_once();
  test_vsync_median();
  test_presents_in_order();
  test_paused_shows_first_frame_once();
  test_flush_returns_every_buffer();
  test_stall_keeps_last_frame();
  test_shown_callback_once_per_start();
  test_frame_without_pts_is_shown();
  test_wait_shown();
  test_wait_shown_counts_dropped_frames();
  test_stats_count_shown_repeated_dropped();
  g_print("all sink tests passed\n");
  return 0;
}
