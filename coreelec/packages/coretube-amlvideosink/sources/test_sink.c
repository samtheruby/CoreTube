/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host test for gstamlvideosink.c: vcodec_profile parsing, AV1 framing, decoder config,
 * and the sysfs effects of opening/closing the decoder (on a fake sysfs tree). */
#define SYSFS_ROOT "/tmp/fakesys"
#define VIDEO_HOLD_TIMEOUT (200 * GST_MSECOND)
#include "gstamlvideosink.c"
#include <assert.h>

static GByteArray *written;
static int fake_write(codec_para_t *c, void *buf, int len) { (void)c; g_byte_array_append(written, buf, len); return len; }

static int init_result, fake_vpts;
static int fake_init(codec_para_t *c) { (void)c; return init_result; }
static int fake_ok(codec_para_t *c) { (void)c; return 0; }
static int fake_checkin(codec_para_t *c, unsigned long pts) { (void)c; (void)pts; return 0; }
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
  g_print("all sink tests passed\n");
  return 0;
}
