/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026-present Team CoreELEC (https://coreelec.org) */

/*
 * amlvideosink hands compressed H.264/H.265/VP9/AV1 frames (the ones the kernel's
 * decoder lists in vcodec_profile) to the Amlogic hardware
 * decoder (libamcodec). The hardware decodes them and shows them on the video
 * layer under the graphics plane, presenting each frame when its own clock
 * (pcrscr) reaches the frame's timestamp. This sink keeps that clock on the
 * pipeline clock and places the video where the "rectangle" property says.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <codec.h>

GST_DEBUG_CATEGORY_STATIC(aml_video_sink_debug);
#define GST_CAT_DEFAULT aml_video_sink_debug

/* The image's libamcodec leaves its audio functions to a library the image does not
 * ship, so it only loads with lazy binding (as Kodi loads it), not as a link-time
 * dependency of a plugin. These are the video functions this sink uses. */
static struct {
  int (*init)(codec_para_t *);
  int (*close)(codec_para_t *);
  int (*reset)(codec_para_t *);
  int (*write)(codec_para_t *, void *, int);
  int (*checkin_pts)(codec_para_t *, unsigned long);
  int (*get_vpts)(codec_para_t *);
  int (*get_pcrscr)(codec_para_t *);
  int (*set_pcrscr)(codec_para_t *, int);
  int (*pause)(codec_para_t *);
  int (*resume)(codec_para_t *);
  int (*set_freerun_mode)(codec_para_t *, unsigned int);
  int (*set_eos)(codec_para_t *, int);
} amcodec;

static gboolean load_amcodec(void)
{
  void *lib = dlopen("libamcodec.so", RTLD_LAZY | RTLD_LOCAL);

  if (!lib) {
    GST_WARNING("cannot load libamcodec: %s", dlerror());
    return FALSE;
  }
#define LOAD(field)                                                    \
  if (!(*(void **)&amcodec.field = dlsym(lib, "codec_" #field))) {     \
    GST_WARNING("libamcodec has no codec_%s", #field);                 \
    dlclose(lib);                                                      \
    return FALSE;                                                      \
  }
  LOAD(init) LOAD(close) LOAD(reset) LOAD(write) LOAD(checkin_pts) LOAD(get_vpts)
  LOAD(get_pcrscr) LOAD(set_pcrscr) LOAD(pause) LOAD(resume) LOAD(set_freerun_mode) LOAD(set_eos)
#undef LOAD
  return TRUE;
}

/* Decoder flag from Amlogic's players (not in the headers): timestamps come from us. */
#define EXTERNAL_PTS 0x1

/* Frames go to the decoder this far ahead of their display time. */
#define LEAD_TIME (500 * GST_MSECOND)
/* Correct the hardware clock when it is further than this from the pipeline clock (90 kHz ticks). */
#define MAX_CLOCK_DRIFT (90000 / 50)
/* Longest wait at end of stream for the last frame to be shown. */
#define EOS_TIMEOUT (2 * G_USEC_PER_SEC)

/* Longest the screen stays muted while a decoder (re)starts. */
#ifndef VIDEO_HOLD_TIMEOUT
#define VIDEO_HOLD_TIMEOUT (3 * GST_SECOND)
#endif

/* The host test points this at a fake sysfs tree. */
#ifndef SYSFS_ROOT
#define SYSFS_ROOT ""
#endif
#define SYSFS_VIDEO_AXIS SYSFS_ROOT "/sys/class/video/axis"
#define SYSFS_DISABLE_VIDEO SYSFS_ROOT "/sys/class/video/disable_video"
#define SYSFS_BLACKOUT_POLICY SYSFS_ROOT "/sys/class/video/blackout_policy"
#define SYSFS_VID_MUTE SYSFS_ROOT "/sys/class/amhdmitx/amhdmitx0/vid_mute"
#define SYSFS_TSYNC_ENABLE SYSFS_ROOT "/sys/class/tsync/enable"
#define SYSFS_TSYNC_MODE SYSFS_ROOT "/sys/class/tsync/mode"
#define SYSFS_FB_PAGE SYSFS_ROOT "/sys/class/graphics/fb0/free_scale_axis"
#define SYSFS_FB_SCREEN SYSFS_ROOT "/sys/class/graphics/fb0/window_axis"
#define SYSFS_VCODEC_PROFILE SYSFS_ROOT "/sys/class/amstream/vcodec_profile"

#define GST_TYPE_AML_VIDEO_SINK (gst_aml_video_sink_get_type())
G_DECLARE_FINAL_TYPE(GstAmlVideoSink, gst_aml_video_sink, GST, AML_VIDEO_SINK, GstBaseSink)

struct _GstAmlVideoSink {
  GstBaseSink parent;

  codec_para_t codec;
  gboolean open;
  gint flushing;   /* atomic: set while the streaming thread must stop waiting */
  guint32 last_pts; /* 90 kHz timestamp of the last frame handed to the decoder */
  gchar *config;    /* decoder config string; codec.config points here while open */
  gint saved_blackout; /* blackout_policy before the decoder opened; -1 when not changed */

  /* Screen mute while a decoder (re)starts; guarded by hold_lock. */
  GMutex hold_lock;
  gboolean held;
  guint32 hold_vpts;  /* decoder clock when the hold started */
  guint32 hold_pts;   /* first frame queued after the hold started; 0 until then */
  GstClockID hold_timeout;

  /* Where the page wants the video, in page pixels; guarded by the object lock. */
  gint rect[4];
  gboolean has_rect;
};

enum { PROP_0, PROP_RECTANGLE };

#define H264_CAPS "video/x-h264, stream-format = (string) byte-stream, alignment = (string) au"
#define H265_CAPS "video/x-h265, stream-format = (string) byte-stream, alignment = (string) au"
#define VP9_CAPS "video/x-vp9"
#define AV1_CAPS "video/x-av1"

/* TRUE if vcodec_profile ("name:feature, feature;" per line) lists the codec and,
 * when given, the feature. Kodi reads the same file with the same rules. */
static gboolean profile_has(const gchar *profile, const gchar *codec, const gchar *feature)
{
  gchar **lines = g_strsplit(profile, "\n", -1);
  gboolean found = FALSE;

  for (gchar **line = lines; *line && !found; line++) {
    const gchar *colon = strchr(*line, ':');

    if (colon && (gsize)(colon - *line) == strlen(codec) && strncmp(*line, codec, strlen(codec)) == 0)
      found = !feature || strstr(colon, feature) != NULL;
  }
  g_strfreev(lines);
  return found;
}

/* The formats this device's decoder takes. Without vcodec_profile, the ones every
 * Amlogic NG decoder has. */
static GstCaps *supported_caps(void)
{
  gchar *profile = NULL;
  GString *caps;
  GstCaps *result;

  if (!g_file_get_contents(SYSFS_VCODEC_PROFILE, &profile, NULL, NULL))
    return gst_caps_from_string(H264_CAPS "; " H265_CAPS "; " VP9_CAPS);

  caps = g_string_new(NULL);
  if (profile_has(profile, "h264", NULL) || profile_has(profile, "mh264", NULL))
    g_string_append(caps, H264_CAPS "; ");
  if (profile_has(profile, "hevc", NULL) || profile_has(profile, "hevc_fb", NULL))
    g_string_append(caps, H265_CAPS "; ");
  /* Kodi only uses VP9 and AV1 when the decoder has compressed output. */
  if (profile_has(profile, "vp9", "compressed") || profile_has(profile, "vp9_fb", "compressed"))
    g_string_append(caps, VP9_CAPS "; ");
  if (profile_has(profile, "av1", "compressed") || profile_has(profile, "av1_fb", "compressed"))
    g_string_append(caps, AV1_CAPS "; ");
  g_free(profile);

  result = caps->len ? gst_caps_from_string(caps->str) : gst_caps_new_empty();
  g_string_free(caps, TRUE);
  return result;
}

G_DEFINE_TYPE(GstAmlVideoSink, gst_aml_video_sink, GST_TYPE_BASE_SINK)

static void write_sysfs(const char *path, const char *value)
{
  FILE *f = fopen(path, "w");

  if (!f) {
    GST_WARNING("cannot open %s: %s", path, g_strerror(errno));
    return;
  }
  if (fputs(value, f) < 0)
    GST_WARNING("cannot write %s: %s", path, g_strerror(errno));
  fclose(f);
}

/* Reads "x0 y0 x1 y1" and returns the width and height it spans. */
static gboolean read_axis_size(const char *path, gint *width, gint *height)
{
  FILE *f = fopen(path, "r");
  gint x0, y0, x1, y1;
  gboolean ok;

  if (!f)
    return FALSE;
  ok = fscanf(f, "%d %d %d %d", &x0, &y0, &x1, &y1) == 4 && x1 > x0 && y1 > y0;
  fclose(f);
  if (ok) {
    *width = x1 - x0 + 1;
    *height = y1 - y0 + 1;
  }
  return ok;
}

/* The page is drawn at the framebuffer scaler's input size and stretched to its
 * output size (see the add-on's tizentube-start), so scale the page rectangle the same way. */
static void apply_rectangle(GstAmlVideoSink *self)
{
  gint r[4], page_w = 1, page_h = 1, screen_w = 1, screen_h = 1;
  gchar axis[64];

  GST_OBJECT_LOCK(self);
  if (!self->has_rect) {
    GST_OBJECT_UNLOCK(self);
    return;
  }
  memcpy(r, self->rect, sizeof(r));
  GST_OBJECT_UNLOCK(self);

  if (!read_axis_size(SYSFS_FB_PAGE, &page_w, &page_h) || !read_axis_size(SYSFS_FB_SCREEN, &screen_w, &screen_h))
    page_w = screen_w = page_h = screen_h = 1;

  g_snprintf(axis, sizeof(axis), "%d %d %d %d",
      (gint)((gint64)r[0] * screen_w / page_w), (gint)((gint64)r[1] * screen_h / page_h),
      (gint)((gint64)(r[0] + r[2]) * screen_w / page_w) - 1, (gint)((gint64)(r[1] + r[3]) * screen_h / page_h) - 1);
  GST_DEBUG_OBJECT(self, "video axis %s", axis);
  write_sysfs(SYSFS_VIDEO_AXIS, axis);
}

static gint read_sysfs_int(const char *path)
{
  FILE *f = fopen(path, "r");
  gint value = -1;

  if (!f)
    return -1;
  if (fscanf(f, "%d", &value) != 1)
    value = -1;
  fclose(f);
  return value;
}

/* TRUE when the video covers the whole page (or the page never placed it). */
static gboolean is_fullscreen(GstAmlVideoSink *self)
{
  gint r[4], page_w, page_h;
  gboolean has_rect;

  GST_OBJECT_LOCK(self);
  has_rect = self->has_rect;
  memcpy(r, self->rect, sizeof(r));
  GST_OBJECT_UNLOCK(self);
  if (!has_rect || !read_axis_size(SYSFS_FB_PAGE, &page_w, &page_h))
    return TRUE;
  return r[0] <= 0 && r[1] <= 0 && r[0] + r[2] >= page_w && r[1] + r[3] >= page_h;
}

/* Ends the screen mute. With id, only when that timeout is still the current one. */
static void release_hold(GstAmlVideoSink *self, GstClockID id)
{
  GstClockID timeout = NULL;

  g_mutex_lock(&self->hold_lock);
  if (self->held && (!id || id == self->hold_timeout)) {
    write_sysfs(SYSFS_VID_MUTE, "0");
    self->held = FALSE;
    timeout = self->hold_timeout;
    self->hold_timeout = NULL;
    GST_DEBUG_OBJECT(self, "screen unmuted");
  }
  g_mutex_unlock(&self->hold_lock);
  /* Outside the lock: the clock thread may be waiting for it in on_hold_timeout. */
  if (timeout) {
    gst_clock_id_unschedule(timeout);
    gst_clock_id_unref(timeout);
  }
}

static gboolean on_hold_timeout(GstClock *clock, GstClockTime time, GstClockID id, gpointer user_data)
{
  GstAmlVideoSink *self = user_data;

  (void)clock;
  (void)time;
  GST_WARNING_OBJECT(self, "no new frame in time; unmuting the screen");
  release_hold(self, id);
  return TRUE;
}

/* Kodi's fix for the green flash at a decoder (re)start (CAMLCodec::HoldVideo): the
 * new decoder briefly writes over the frame still on screen, so mute the HDMI
 * output until a new frame shows, or VIDEO_HOLD_TIMEOUT at most. Only for a
 * fullscreen video: the mute blanks the whole screen, page included. */
static void hold_video(GstAmlVideoSink *self)
{
  GstClock *clock;
  GstClockID timeout;

  if (!is_fullscreen(self))
    return;
  release_hold(self, NULL);
  clock = gst_system_clock_obtain();
  timeout = gst_clock_new_single_shot_id(clock, gst_clock_get_time(clock) + VIDEO_HOLD_TIMEOUT);
  gst_object_unref(clock);

  g_mutex_lock(&self->hold_lock);
  write_sysfs(SYSFS_VID_MUTE, "1");
  self->held = TRUE;
  self->hold_vpts = self->open ? (guint32)amcodec.get_vpts(&self->codec) : 0;
  self->hold_pts = 0;
  self->hold_timeout = timeout;
  g_mutex_unlock(&self->hold_lock);
  if (gst_clock_id_wait_async(timeout, on_hold_timeout, gst_object_ref(self), gst_object_unref) != GST_CLOCK_OK)
    release_hold(self, NULL);
  GST_DEBUG_OBJECT(self, "screen muted until the first new frame");
}

/* Called for each frame queued: unmute once the decoder shows the first frame
 * queued since the hold began. The decoder clock must also have moved, as the
 * previous video may have left it past the new timestamps. */
static void check_hold(GstAmlVideoSink *self, guint32 queued_pts)
{
  gboolean shown;
  guint32 vpts;

  g_mutex_lock(&self->hold_lock);
  if (!self->held) {
    g_mutex_unlock(&self->hold_lock);
    return;
  }
  if (!self->hold_pts)
    self->hold_pts = queued_pts;
  vpts = (guint32)amcodec.get_vpts(&self->codec);
  shown = self->hold_pts && vpts != self->hold_vpts && (gint32)(vpts - self->hold_pts) >= 0;
  g_mutex_unlock(&self->hold_lock);
  if (shown)
    release_hold(self, NULL);
}

/* Running time to the decoder's 90 kHz clock; +1 keeps 0, which amcodec treats as "no time", unused. */
static guint32 to_pts(GstClockTime running_time)
{
  return (guint32)(gst_util_uint64_scale(running_time, 90000, GST_SECOND) + 1);
}

static void sync_hardware_clock(GstAmlVideoSink *self, gboolean force)
{
  GstElement *element = GST_ELEMENT(self);
  GstClock *clock = gst_element_get_clock(element);
  GstClockTime now, base_time;
  guint32 target, current;

  if (!clock)
    return;
  now = gst_clock_get_time(clock);
  gst_object_unref(clock);
  base_time = gst_element_get_base_time(element);
  if (now < base_time)
    return;

  target = to_pts(now - base_time);
  current = (guint32)amcodec.get_pcrscr(&self->codec);
  if (force || ABS((gint32)(target - current)) > MAX_CLOCK_DRIFT) {
    GST_LOG_OBJECT(self, "hardware clock %u -> %u", current, target);
    amcodec.set_pcrscr(&self->codec, (int)target);
  }
}

static void restore_blackout(GstAmlVideoSink *self)
{
  gchar value[16];

  if (self->saved_blackout < 0)
    return;
  g_snprintf(value, sizeof(value), "%d", self->saved_blackout);
  write_sysfs(SYSFS_BLACKOUT_POLICY, value);
  self->saved_blackout = -1;
}

static void close_decoder(GstAmlVideoSink *self)
{
  g_clear_pointer(&self->config, g_free);
  if (!self->open)
    return;
  release_hold(self, NULL);
  amcodec.close(&self->codec);
  self->open = FALSE;
  write_sysfs(SYSFS_DISABLE_VIDEO, "1");
  /* After the close: blackout_policy 1 made the kernel drop the last frame there. */
  restore_blackout(self);
}

/* The decoder's config string, as Kodi's CAMLCodec::GetHDRStaticMetadata builds it:
 * the picture size for AV1, the HDR metadata for VP9 (the VP9 stream does not carry
 * it; HEVC and AV1 streams do). NULL when there is nothing to send. */
static gchar *decoder_config(vformat_t format, GstCaps *caps, gint width, gint height)
{
  GstStructure *s = gst_caps_get_structure(caps, 0);
  GstVideoMasteringDisplayInfo mdi;
  GstVideoContentLightLevel cll;
  GstVideoColorimetry colorimetry;
  const gchar *colorimetry_str;
  GString *config;

  if (format == VFORMAT_AV1)
    return width > 0 && height > 0 ? g_strdup_printf("av1_max_pic_w:%d;av1_max_pic_h:%d;", width, height) : NULL;
  if (format != VFORMAT_VP9 || !gst_video_mastering_display_info_from_caps(&mdi, caps))
    return NULL;

  /* CTA-861.3 units, which GStreamer uses too: chromaticity in 1/50000, luminance in 1/10000 cd/m2. */
  config = g_string_new("HDRStaticInfo:1");
  g_string_append_printf(config, ";mR.x:%u;mR.y:%u;mG.x:%u;mG.y:%u;mB.x:%u;mB.y:%u;mW.x:%u;mW.y:%u",
      mdi.display_primaries[0].x, mdi.display_primaries[0].y, mdi.display_primaries[1].x,
      mdi.display_primaries[1].y, mdi.display_primaries[2].x, mdi.display_primaries[2].y,
      mdi.white_point.x, mdi.white_point.y);
  g_string_append_printf(config, ";mMaxDL:%u;mMinDL:%u", mdi.max_display_mastering_luminance,
      mdi.min_display_mastering_luminance);
  if (gst_video_content_light_level_from_caps(&cll, caps))
    g_string_append_printf(config, ";mCLLPresent:1;mMaxCLL:%u;mMaxFALL:%u", cll.max_content_light_level,
        cll.max_frame_average_light_level);
  /* Kodi sends the FFmpeg transfer number: 16 = SMPTE ST 2084 (PQ), 18 = ARIB STD-B67 (HLG). */
  colorimetry_str = gst_structure_get_string(s, "colorimetry");
  if (colorimetry_str && gst_video_colorimetry_from_string(&colorimetry, colorimetry_str)) {
    if (colorimetry.transfer == GST_VIDEO_TRANSFER_SMPTE2084)
      g_string_append(config, ";mTransfer:16");
    else if (colorimetry.transfer == GST_VIDEO_TRANSFER_ARIB_STD_B67)
      g_string_append(config, ";mTransfer:18");
  }
  return g_string_free(config, FALSE);
}

static gboolean open_decoder(GstAmlVideoSink *self, GstCaps *caps)
{
  GstStructure *s = gst_caps_get_structure(caps, 0);
  gint width = 0, height = 0, fps_n = 0, fps_d = 1, ret;

  close_decoder(self);
  memset(&self->codec, 0, sizeof(self->codec));
  self->codec.handle = self->codec.cntl_handle = self->codec.sub_handle = self->codec.audio_utils_handle = -1;
  self->codec.has_video = 1;
  self->codec.noblock = 1;
  self->codec.stream_type = STREAM_TYPE_ES_VIDEO;
  self->codec.dec_mode = STREAM_TYPE_FRAME;
  /* Straight to the video layer ("vdec amvideo"). The amlvideo path waits for an app to
   * dequeue frames over V4L2, as Kodi does, and the kernel has no map for FRAME_BASE_PATH_AMVIDEO. */
  self->codec.video_path = FRAME_BASE_PATH_TUNNEL_MODE;
  self->codec.am_sysinfo.param = (void *)EXTERNAL_PTS;

  if (gst_structure_has_name(s, "video/x-h264")) {
    self->codec.video_type = VFORMAT_H264;
    self->codec.am_sysinfo.format = VIDEO_DEC_FORMAT_H264;
  } else if (gst_structure_has_name(s, "video/x-h265")) {
    self->codec.video_type = VFORMAT_HEVC;
    self->codec.am_sysinfo.format = VIDEO_DEC_FORMAT_HEVC;
  } else if (gst_structure_has_name(s, "video/x-av1")) {
    self->codec.video_type = VFORMAT_AV1;
    self->codec.am_sysinfo.format = VIDEO_DEC_FORMAT_AV1;
  } else {
    self->codec.video_type = VFORMAT_VP9;
    self->codec.am_sysinfo.format = VIDEO_DEC_FORMAT_VP9;
  }
  gst_structure_get_int(s, "width", &width);
  gst_structure_get_int(s, "height", &height);
  self->config = decoder_config(self->codec.video_type, caps, width, height);
  if (self->config) {
    self->codec.config = self->config;
    self->codec.config_len = (int)strlen(self->config);
    GST_DEBUG_OBJECT(self, "decoder config %s", self->config);
  }
  gst_structure_get_fraction(s, "framerate", &fps_n, &fps_d);
  self->codec.am_sysinfo.width = width;
  self->codec.am_sysinfo.height = height;
  /* Frame duration in 1/96000 s; 0 lets the decoder work it out. */
  self->codec.am_sysinfo.rate = fps_n > 0 ? (unsigned int)gst_util_uint64_scale_int(96000, fps_d, fps_n) : 0;

  /* The hardware presents frames by its own clock, which sync_hardware_clock() steers. */
  write_sysfs(SYSFS_TSYNC_ENABLE, "1");
  write_sysfs(SYSFS_TSYNC_MODE, "0");
  /* As Kodi plays: with blackout_policy 0 the kernel keeps the last frame on the
   * video layer when the decoder closes, and turns that layer back on even after
   * disable_video=1; an HDR frame left there keeps the TV in HDR under Kodi. */
  self->saved_blackout = read_sysfs_int(SYSFS_BLACKOUT_POLICY);
  if (self->saved_blackout >= 0)
    write_sysfs(SYSFS_BLACKOUT_POLICY, "1");

  ret = amcodec.init(&self->codec);
  if (ret != 0) {
    restore_blackout(self);
    GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, ("Cannot open the hardware video decoder."),
        ("codec_init failed: %d", ret));
    return FALSE;
  }
  self->open = TRUE;
  amcodec.set_freerun_mode(&self->codec, 0);
  /* 2 = VIDEO_DISABLE_FORNEXT: the video plane comes on with the first decoded
   * frame; on now it would show leftover memory (green/pink) until then. */
  write_sysfs(SYSFS_DISABLE_VIDEO, "2");
  apply_rectangle(self);
  hold_video(self);
  if (GST_STATE(self) != GST_STATE_PLAYING)
    amcodec.pause(&self->codec);
  GST_INFO_OBJECT(self, "decoder open: %" GST_PTR_FORMAT, caps);
  return TRUE;
}

static gboolean gst_aml_video_sink_set_caps(GstBaseSink *bsink, GstCaps *caps)
{
  GstAmlVideoSink *self = GST_AML_VIDEO_SINK(bsink);
  GstCaps *current = gst_pad_get_current_caps(GST_BASE_SINK_PAD(bsink));
  gboolean same_codec = FALSE;

  /* A new resolution of the same codec needs no new decoder. */
  if (current) {
    same_codec = gst_structure_has_name(gst_caps_get_structure(caps, 0),
        gst_structure_get_name(gst_caps_get_structure(current, 0)));
    gst_caps_unref(current);
  }
  if (self->open && same_codec)
    return TRUE;
  return open_decoder(self, caps);
}

static GstFlowReturn write_all(GstAmlVideoSink *self, const guint8 *data, gsize size)
{
  gsize done = 0;

  while (done < size) {
    gint written = amcodec.write(&self->codec, (void *)(data + done), (int)(size - done));

    if (written > 0) {
      done += (gsize)written;
    } else if (written < 0 && errno != EAGAIN && errno != EINTR) {
      GST_ELEMENT_ERROR(self, RESOURCE, WRITE, ("The hardware video decoder failed."),
          ("codec_write failed: %s", g_strerror(errno)));
      return GST_FLOW_ERROR;
    } else if (g_atomic_int_get(&self->flushing)) {
      return GST_FLOW_FLUSHING;
    } else {
      g_usleep(5000); /* decoder input is full */
    }
  }
  return GST_FLOW_OK;
}

/* Amlogic's VP9 decoder takes each frame of a superframe behind a 16-byte header:
 * size + 4, the same inverted, 00 00 00 01, "AMLV" (as Kodi writes it). In frame mode
 * every write is one packet, so the rebuilt packet goes out in a single write. */
static GstFlowReturn write_vp9(GstAmlVideoSink *self, const guint8 *data, gsize size)
{
  gsize sizes[8] = {size}, frames = 1, offset = 0;
  guint8 marker = size > 0 ? data[size - 1] : 0;
  GByteArray *packet;
  GstFlowReturn ret;

  if ((marker & 0xe0) == 0xc0) {
    gsize count = (marker & 0x7) + 1, mag = ((marker >> 3) & 0x3) + 1, index = 2 + mag * count, total = 0;

    if (size >= index && data[size - index] == marker) {
      const guint8 *p = data + size - index + 1;

      for (gsize i = 0; i < count; i++) {
        sizes[i] = 0;
        for (gsize b = 0; b < mag; b++)
          sizes[i] |= (gsize)*p++ << (8 * b);
        total += sizes[i];
      }
      if (total <= size - index)
        frames = count;
      else
        sizes[0] = size; /* broken index: send it whole */
    }
  }

  packet = g_byte_array_sized_new((guint)(size + 16 * frames));
  for (gsize i = 0; i < frames; i++) {
    guint32 len = (guint32)sizes[i] + 4;
    guint8 header[16] = {len >> 24, len >> 16, len >> 8, len, ~len >> 24, ~len >> 16, ~len >> 8, ~len,
                         0, 0, 0, 1, 'A', 'M', 'L', 'V'};

    g_byte_array_append(packet, header, sizeof(header));
    g_byte_array_append(packet, data + offset, (guint)sizes[i]);
    offset += sizes[i];
  }
  ret = write_all(self, packet->data, packet->len);
  g_byte_array_unref(packet);
  return ret;
}

/* Amlogic's AV1 decoder takes each OBU behind a 20-byte header: size + 4, the same
 * inverted, 00 00 00 01, "AMLV", then size as a 4-byte LEB128, where size is the
 * OBU's length + 4 (as Kodi's av1_parser_frame writes it). The OBUs come in the
 * low-overhead format MP4 and WebM use; one without a size field runs to the end. */
static GstFlowReturn write_av1(GstAmlVideoSink *self, const guint8 *data, gsize size)
{
  GByteArray *packet = g_byte_array_sized_new((guint)(size + 256));
  GstFlowReturn ret;
  gsize offset = 0;

  while (offset < size) {
    const guint8 *obu = data + offset;
    gsize left = size - offset, header = (obu[0] & 0x04) ? 2 : 1, payload = 0, length;
    guint8 amlv[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 'A', 'M', 'L', 'V'};
    guint32 obu_size;

    if (obu[0] & 0x02) { /* obu_has_size_field: LEB128, at most 8 bytes */
      gsize i = 0;

      for (; i < 8 && header + i < left; i++) {
        payload |= (gsize)(obu[header + i] & 0x7f) << (7 * i);
        if (!(obu[header + i] & 0x80))
          break;
      }
      if (i == 8 || header + i >= left)
        payload = G_MAXSIZE; /* size field runs past the buffer */
      header += i + 1;
    } else {
      payload = left > header ? left - header : 0;
    }
    if ((obu[0] & 0x80) || header > left || payload > left - header) {
      GST_WARNING_OBJECT(self, "dropping AV1 frame with a broken OBU at byte %" G_GSIZE_FORMAT, offset);
      g_byte_array_unref(packet);
      return GST_FLOW_OK;
    }

    length = header + payload;
    obu_size = (guint32)length + 4;
    for (gsize b = 0; b < 4; b++) {
      amlv[b] = (guint8)((obu_size + 4) >> (24 - 8 * b));
      amlv[4 + b] = (guint8)~amlv[b];
      amlv[16 + b] = (guint8)(((obu_size >> (7 * b)) & 0x7f) | (b < 3 ? 0x80 : 0));
    }
    g_byte_array_append(packet, amlv, sizeof(amlv));
    g_byte_array_append(packet, obu, (guint)length);
    offset += length;
  }
  ret = write_all(self, packet->data, packet->len);
  g_byte_array_unref(packet);
  return ret;
}

static GstFlowReturn gst_aml_video_sink_render(GstBaseSink *bsink, GstBuffer *buffer)
{
  GstAmlVideoSink *self = GST_AML_VIDEO_SINK(bsink);
  GstClockTime running_time;
  GstFlowReturn ret;
  GstMapInfo map;

  if (!self->open)
    return GST_FLOW_NOT_NEGOTIATED;

  sync_hardware_clock(self, FALSE);

  running_time = gst_segment_to_running_time(&bsink->segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
  if (GST_CLOCK_TIME_IS_VALID(running_time)) {
    self->last_pts = to_pts(running_time);
    if (amcodec.checkin_pts(&self->codec, self->last_pts) != 0)
      GST_WARNING_OBJECT(self, "cannot queue timestamp %u", self->last_pts);
    check_hold(self, self->last_pts);
  }

  if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    return GST_FLOW_ERROR;
  if (self->codec.video_type == VFORMAT_VP9)
    ret = write_vp9(self, map.data, map.size);
  else if (self->codec.video_type == VFORMAT_AV1)
    ret = write_av1(self, map.data, map.size);
  else
    ret = write_all(self, map.data, map.size);
  gst_buffer_unmap(buffer, &map);
  return ret;
}

/* At end of stream, wait until the last queued frame is on screen. */
static void wait_for_last_frame(GstAmlVideoSink *self)
{
  gint64 deadline = g_get_monotonic_time() + EOS_TIMEOUT;

  /* The decoder holds its last frames for reordering until it knows the stream ended. */
  if (self->open)
    amcodec.set_eos(&self->codec, 1);
  while (self->open && !g_atomic_int_get(&self->flushing) && g_get_monotonic_time() < deadline &&
         (gint32)((guint32)amcodec.get_vpts(&self->codec) - self->last_pts) < 0)
    g_usleep(10000);
  GST_DEBUG_OBJECT(self, "end of stream: shown %u, last %u", self->open ? (guint32)amcodec.get_vpts(&self->codec) : 0,
      self->last_pts);
}

static gboolean gst_aml_video_sink_event(GstBaseSink *bsink, GstEvent *event)
{
  GstAmlVideoSink *self = GST_AML_VIDEO_SINK(bsink);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_STOP:
      /* Drop what the decoder still holds; the next frames start a new timeline. */
      if (self->open) {
        hold_video(self);
        amcodec.reset(&self->codec);
        if (GST_STATE(self) != GST_STATE_PLAYING)
          amcodec.pause(&self->codec);
      }
      self->last_pts = 0;
      break;
    case GST_EVENT_EOS:
      wait_for_last_frame(self);
      break;
    default:
      break;
  }
  return GST_BASE_SINK_CLASS(gst_aml_video_sink_parent_class)->event(bsink, event);
}

static gboolean gst_aml_video_sink_unlock(GstBaseSink *bsink)
{
  g_atomic_int_set(&GST_AML_VIDEO_SINK(bsink)->flushing, 1);
  return TRUE;
}

static gboolean gst_aml_video_sink_unlock_stop(GstBaseSink *bsink)
{
  g_atomic_int_set(&GST_AML_VIDEO_SINK(bsink)->flushing, 0);
  return TRUE;
}

static GstStateChangeReturn gst_aml_video_sink_change_state(GstElement *element, GstStateChange transition)
{
  GstAmlVideoSink *self = GST_AML_VIDEO_SINK(element);
  GstStateChangeReturn ret;

  if (transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED && self->open)
    amcodec.pause(&self->codec);

  ret = GST_ELEMENT_CLASS(gst_aml_video_sink_parent_class)->change_state(element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  if (transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING && self->open) {
    sync_hardware_clock(self, TRUE);
    amcodec.resume(&self->codec);
  } else if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    close_decoder(self);
  }
  return ret;
}

static void gst_aml_video_sink_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  GstAmlVideoSink *self = GST_AML_VIDEO_SINK(object);
  const gchar *str;
  gint r[4];

  switch (prop_id) {
    case PROP_RECTANGLE:
      str = g_value_get_string(value);
      if (!str || sscanf(str, "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) != 4 || r[2] <= 0 || r[3] <= 0) {
        GST_WARNING_OBJECT(self, "ignoring rectangle \"%s\", expected x,y,width,height", GST_STR_NULL(str));
        break;
      }
      GST_OBJECT_LOCK(self);
      memcpy(self->rect, r, sizeof(r));
      self->has_rect = TRUE;
      GST_OBJECT_UNLOCK(self);
      apply_rectangle(self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_aml_video_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GstAmlVideoSink *self = GST_AML_VIDEO_SINK(object);

  switch (prop_id) {
    case PROP_RECTANGLE:
      GST_OBJECT_LOCK(self);
      if (self->has_rect)
        g_value_take_string(value,
            g_strdup_printf("%d,%d,%d,%d", self->rect[0], self->rect[1], self->rect[2], self->rect[3]));
      else
        g_value_set_string(value, NULL);
      GST_OBJECT_UNLOCK(self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_aml_video_sink_finalize(GObject *object)
{
  GstAmlVideoSink *self = GST_AML_VIDEO_SINK(object);

  release_hold(self, NULL);
  g_mutex_clear(&self->hold_lock);
  G_OBJECT_CLASS(gst_aml_video_sink_parent_class)->finalize(object);
}

static void gst_aml_video_sink_class_init(GstAmlVideoSinkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS(klass);
  GstCaps *caps;

  gobject_class->set_property = gst_aml_video_sink_set_property;
  gobject_class->get_property = gst_aml_video_sink_get_property;
  gobject_class->finalize = gst_aml_video_sink_finalize;
  g_object_class_install_property(gobject_class, PROP_RECTANGLE,
      g_param_spec_string("rectangle", "Rectangle",
          "Video position in page pixels as \"x,y,width,height\"", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  element_class->change_state = gst_aml_video_sink_change_state;
  caps = supported_caps();
  gst_element_class_add_pad_template(element_class, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
  gst_element_class_set_static_metadata(element_class, "Amlogic video sink", "Sink/Video/Hardware",
      "Decodes and shows video with the Amlogic hardware decoder", "Team CoreELEC");

  basesink_class->set_caps = gst_aml_video_sink_set_caps;
  basesink_class->render = gst_aml_video_sink_render;
  basesink_class->event = gst_aml_video_sink_event;
  basesink_class->unlock = gst_aml_video_sink_unlock;
  basesink_class->unlock_stop = gst_aml_video_sink_unlock_stop;
}

static void gst_aml_video_sink_init(GstAmlVideoSink *self)
{
  GstBaseSink *bsink = GST_BASE_SINK(self);

  self->saved_blackout = -1;
  g_mutex_init(&self->hold_lock);

  /* Hand each frame over ahead of its time, never drop it: the decoder needs every frame. */
  gst_base_sink_set_sync(bsink, TRUE);
  gst_base_sink_set_ts_offset(bsink, -(GstClockTimeDiff)LEAD_TIME);
  gst_base_sink_set_max_lateness(bsink, -1);
  gst_base_sink_set_qos_enabled(bsink, FALSE);
}

static gboolean plugin_init(GstPlugin *plugin)
{
  GST_DEBUG_CATEGORY_INIT(aml_video_sink_debug, "amlvideosink", 0, "Amlogic video sink");
  if (!load_amcodec())
    return FALSE;
  return gst_element_register(plugin, "amlvideosink", GST_RANK_NONE, GST_TYPE_AML_VIDEO_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, amlvideosink, "Amlogic hardware video sink", plugin_init,
    VERSION, "LGPL", "gst-amlvideosink", "https://coreelec.org")
