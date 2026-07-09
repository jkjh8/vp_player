#include "media/probe.h"

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <optional>
#include <string>
#include <thread>

namespace vp {

using json = nlohmann::json;

namespace {

std::optional<std::string> ToUri(const std::string& path) {
  GError* err = nullptr;
  gchar* uri = gst_filename_to_uri(path.c_str(), &err);
  if (!uri) {
    if (err) g_error_free(err);
    return std::nullopt;
  }
  std::string out(uri);
  g_free(uri);
  return out;
}

// GStreamer caps 이름 → ffprobe 스타일 codec_name (표시용 근사).
std::string CapsToCodecName(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) return "unknown";
  const GstStructure* s = gst_caps_get_structure(caps, 0);
  const char* name = gst_structure_get_name(s);
  std::string n = name ? name : "";

  if (n == "audio/mpeg") {
    int v = 0;
    gst_structure_get_int(s, "mpegversion", &v);
    if (v == 1) {
      int layer = 0;
      gst_structure_get_int(s, "layer", &layer);
      return layer == 3 ? "mp3" : "mp2";
    }
    return "aac";  // mpegversion 2/4
  }
  if (n == "video/x-h264") return "h264";
  if (n == "video/x-h265") return "hevc";
  if (n == "video/x-vp8") return "vp8";
  if (n == "video/x-vp9") return "vp9";
  if (n == "video/x-av1") return "av1";
  if (n == "audio/x-raw") return "pcm";
  if (n == "audio/x-opus") return "opus";
  if (n == "audio/x-vorbis") return "vorbis";
  if (n == "audio/x-flac") return "flac";
  if (n == "audio/x-ac3") return "ac3";
  // "video/x-..." / "audio/x-..." 접두어 제거, 아니면 원본
  for (const char* pfx : {"video/x-", "audio/x-", "video/", "audio/"}) {
    if (n.rfind(pfx, 0) == 0) return n.substr(std::string(pfx).size());
  }
  return n;
}

json ProbeSync(const std::string& path) {
  auto uri = ToUri(path);
  if (!uri) return json{{"error", "invalid path"}};

  GError* err = nullptr;
  GstDiscoverer* disc = gst_discoverer_new(20 * GST_SECOND, &err);
  if (!disc) {
    std::string msg = err ? err->message : "discoverer init failed";
    if (err) g_error_free(err);
    return json{{"error", msg}};
  }

  GstDiscovererInfo* info = gst_discoverer_discover_uri(disc, uri->c_str(), &err);
  if (!info || gst_discoverer_info_get_result(info) != GST_DISCOVERER_OK) {
    std::string msg = err ? err->message : "discover failed";
    if (err) g_error_free(err);
    if (info) gst_discoverer_info_unref(info);
    gst_object_unref(disc);
    return json{{"error", msg}};
  }

  const GstClockTime dur = gst_discoverer_info_get_duration(info);
  const double dur_sec = GST_CLOCK_TIME_IS_VALID(dur) ? (double)dur / GST_SECOND : 0.0;

  json format = {{"duration", std::to_string(dur_sec)}, {"nb_streams", 0}};
  // 컨테이너 포맷명
  GstDiscovererStreamInfo* sinfo = gst_discoverer_info_get_stream_info(info);
  if (sinfo) {
    GstCaps* caps = gst_discoverer_stream_info_get_caps(sinfo);
    if (caps) {
      gchar* cs = gst_caps_to_string(caps);
      format["format_name"] = gst_structure_get_name(gst_caps_get_structure(caps, 0));
      if (cs) g_free(cs);
      gst_caps_unref(caps);
    }
    gst_discoverer_stream_info_unref(sinfo);
  }

  json streams = json::array();

  GList* vlist = gst_discoverer_info_get_video_streams(info);
  for (GList* it = vlist; it; it = it->next) {
    auto* vi = (GstDiscovererVideoInfo*)it->data;
    GstCaps* caps = gst_discoverer_stream_info_get_caps((GstDiscovererStreamInfo*)vi);
    json s = {{"codec_type", "video"},
              {"codec_name", CapsToCodecName(caps)},
              {"width", (int)gst_discoverer_video_info_get_width(vi)},
              {"height", (int)gst_discoverer_video_info_get_height(vi)}};
    const guint br = gst_discoverer_video_info_get_bitrate(vi);
    if (br) s["bit_rate"] = std::to_string(br);
    const guint fn = gst_discoverer_video_info_get_framerate_num(vi);
    const guint fd = gst_discoverer_video_info_get_framerate_denom(vi);
    if (fd) s["r_frame_rate"] = std::to_string(fn) + "/" + std::to_string(fd);
    if (caps) gst_caps_unref(caps);
    streams.push_back(s);
  }
  gst_discoverer_stream_info_list_free(vlist);

  GList* alist = gst_discoverer_info_get_audio_streams(info);
  for (GList* it = alist; it; it = it->next) {
    auto* ai = (GstDiscovererAudioInfo*)it->data;
    GstCaps* caps = gst_discoverer_stream_info_get_caps((GstDiscovererStreamInfo*)ai);
    const guint ch = gst_discoverer_audio_info_get_channels(ai);
    json s = {{"codec_type", "audio"},
              {"codec_name", CapsToCodecName(caps)},
              {"channels", (int)ch},
              {"sample_rate", std::to_string(gst_discoverer_audio_info_get_sample_rate(ai))}};
    const guint br = gst_discoverer_audio_info_get_bitrate(ai);
    if (br) s["bit_rate"] = std::to_string(br);
    // 간단한 channel_layout 표기 (표시용)
    s["channel_layout"] = ch == 1 ? "mono" : ch == 2 ? "stereo" : std::to_string(ch) + " channels";
    if (caps) gst_caps_unref(caps);
    streams.push_back(s);
  }
  gst_discoverer_stream_info_list_free(alist);

  format["nb_streams"] = (int)streams.size();

  gst_discoverer_info_unref(info);
  gst_object_unref(disc);
  return json{{"format", format}, {"streams", streams}};
}

// 스냅샷 파이프라인 실행 (워커 스레드). 성공 시 true.
bool ThumbnailSync(const std::string& path, const std::string& out, bool is_image, double at_sec,
                   int width, std::string* err_out) {
  auto uri = ToUri(path);
  if (!uri) {
    *err_out = "invalid path";
    return false;
  }

  // pngenc snapshot=true → 첫 프레임 인코딩 후 EOS. videoscale은 width만 고정하고
  // 높이는 DAR로 자동. 정사각 픽셀 보장 위해 pixel-aspect-ratio=1/1.
  // filesink location은 여기 문자열에 넣지 않는다 — Windows 백슬래시 경로를
  // gst_parse_launch가 이스케이프로 먹어버리므로 아래에서 프로퍼티로 설정.
  std::string desc = "uridecodebin uri=\"" + *uri +
                     "\" ! videoconvert ! videoscale ! video/x-raw,width=" +
                     std::to_string(width) +
                     ",pixel-aspect-ratio=1/1 ! pngenc snapshot=true ! filesink name=vpsink";

  GError* perr = nullptr;
  GstElement* pipe = gst_parse_launch(desc.c_str(), &perr);
  if (!pipe) {
    *err_out = perr ? perr->message : "parse_launch failed";
    if (perr) g_error_free(perr);
    return false;
  }

  // filesink location 프로퍼티 설정 (백슬래시 이스케이프 문제 회피)
  {
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "vpsink");
    if (sink) {
      g_object_set(sink, "location", out.c_str(), nullptr);
      gst_object_unref(sink);
    }
  }

  GstBus* bus = gst_element_get_bus(pipe);
  bool ok = false;

  // 비디오는 at_sec 지점 프레임: PAUSED 프리롤 → seek → PLAYING → EOS.
  // 이미지는 바로 PLAYING → EOS.
  if (!is_image) {
    gst_element_set_state(pipe, GST_STATE_PAUSED);
    GstMessage* m = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR));
    if (m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
      gst_message_unref(m);
      goto done;
    }
    if (m) gst_message_unref(m);
    gst_element_seek_simple(pipe, GST_FORMAT_TIME,
                            (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                            (gint64)(at_sec * GST_SECOND));
  }

  gst_element_set_state(pipe, GST_STATE_PLAYING);
  {
    GstMessage* m = gst_bus_timed_pop_filtered(
        bus, 15 * GST_SECOND, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (m) {
      ok = GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
      if (!ok) {
        GError* e = nullptr;
        gst_message_parse_error(m, &e, nullptr);
        *err_out = e ? e->message : "pipeline error";
        if (e) g_error_free(e);
      }
      gst_message_unref(m);
    } else {
      *err_out = "thumbnail timeout";
    }
  }

done:
  gst_element_set_state(pipe, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipe);
  return ok;
}

}  // namespace

void ProbeMediaAsync(const std::string& path, std::function<void(json)> cb) {
  std::thread([path, cb = std::move(cb)]() { cb(ProbeSync(path)); }).detach();
}

void MakeThumbnailAsync(const std::string& path, const std::string& out, bool is_image,
                        double at_sec, int width,
                        std::function<void(bool, std::string)> cb) {
  std::thread([=, cb = std::move(cb)]() {
    std::string err;
    const bool ok = ThumbnailSync(path, out, is_image, at_sec, width, &err);
    cb(ok, err);
  }).detach();
}

}  // namespace vp
