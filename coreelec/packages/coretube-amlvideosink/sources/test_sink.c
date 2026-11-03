/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host test for gstamlvideosink.c: vcodec_profile parsing, AV1 framing, decoder config,
 * and the sysfs effects of opening/closing the decoder (on a fake sysfs tree). */
#define SYSFS_ROOT "/tmp/fakesys"
#define VIDEO_HOLD_TIMEOUT (200 * GST_MSECOND)
#include "gstamlvideosink.c"
#include "aml_pacer.c"
#include <assert.h>

static GByteArray *written;
static int fake_write(codec_para_t *c, void *buf, int len) { (void)c; g_byte_array_append(written, buf, len); return len; }

static int init_result, fake_vpts;
static unsigned long checkins[4]; /* timestamps queued since the test cleared checkin_count */
static guint checkin_count;
static int fake_init(codec_para_t *c) { (void)c; return init_result; }
static int fake_ok(codec_para_t *c) { (void)c; return 0; }
static int fake_checkin(codec_para_t *c, unsigned long pts)
{
  (void)c;
  if (checkin_count < G_N_ELEMENTS(checkins))
    checkins[checkin_count] = pts;
  checkin_count++;
  return 0;
}
static int fake_get_vpts(codec_para_t *c) { (void)c; return fake_vpts; }
static int fake_set_int(codec_para_t *c, int v) { (void)c; (void)v; return 0; }
static int fake_set_uint(codec_para_t *c, unsigned int v) { (void)c; (void)v; return 0; }

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
  amcodec.init = fake_init;
  amcodec.close = amcodec.reset = amcodec.pause = amcodec.resume = fake_ok;
  amcodec.checkin_pts = fake_checkin;
  amcodec.get_vpts = fake_get_vpts;
  amcodec.set_freerun_mode = fake_set_uint;
  amcodec.set_eos = amcodec.set_pcrscr = fake_set_int;
  amcodec.get_pcrscr = fake_ok;

  sink = g_object_new(GST_TYPE_AML_VIDEO_SINK, NULL);
  gst_segment_init(&GST_BASE_SINK(sink)->segment, GST_FORMAT_TIME);

  /* Fullscreen video: the kernel must not keep the last frame, and the screen is
   * muted until the decoder shows the first frame queued. */
  g_object_set(sink, "rectangle", "0,0,1920,1080", NULL);
  fake_vpts = 0;
  assert(open_decoder(sink, h264));
  assert(is(BLACKOUT, "1") && is(MUTE, "1") && is(DISABLE, "2"));
  render_at(sink, GST_SECOND);
  assert(is(MUTE, "1"));                  /* nothing shown yet */
  fake_vpts = (int)to_pts(GST_SECOND);
  render_at(sink, 2 * GST_SECOND);
  assert(is(MUTE, "0"));                  /* first frame shown */

  /* A seek mutes again; a decoder clock left over from before does not count as shown. */
  fake_vpts = 5000000;
  gst_aml_video_sink_event(GST_BASE_SINK(sink), gst_event_new_flush_stop(TRUE));
  assert(is(MUTE, "1"));
  render_at(sink, GST_SECOND);
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
  assert(checkin_count == 2);              /* timed at the target, so it never shows before it */
  assert(checkins[0] == to_pts(0) && checkins[1] == to_pts(0));

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
  assert(checkin_count == 2 && checkins[0] == to_pts(0) && checkins[1] == to_pts(0));
  assert(gst_element_get_state(sink, NULL, NULL, 0) == GST_STATE_CHANGE_ASYNC); /* no preroll yet */
  gst_element_set_state(sink, GST_STATE_READY); /* unblocks the thread if it hangs */
  g_thread_join(thread);

  assert(gst_element_set_state(sink, GST_STATE_PAUSED) == GST_STATE_CHANGE_ASYNC);
  assert(gst_pad_send_event(pad, gst_event_new_stream_start("seek")));
  assert(gst_pad_send_event(pad, gst_event_new_caps(h264)));
  assert(gst_pad_send_event(pad, gst_event_new_segment(&segment)));
  g_object_set_data(G_OBJECT(pad), "pts", target);
  thread = g_thread_new("target", push_buffers, pad);
  assert(gst_element_get_state(sink, NULL, NULL, GST_SECOND) == GST_STATE_CHANGE_SUCCESS);

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
  test_decoder_sysfs();
  test_seek_inside_gop();
  test_preroll_after_seek();
  test_cadence_24_on_60();
  test_cadence_23976_on_24();
  test_late_frames_dropped();
  test_before_segment_dropped();
  test_nothing_due();
  test_double_speed_shows_each_frame_once();
  test_vsync_median();
  g_print("all sink tests passed\n");
  return 0;
}
