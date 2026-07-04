#include "player/player_core.h"

#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>

#include <lunasvg.h>

namespace vp {

using json = nlohmann::json;

namespace {

constexpr int kCanvasWidth = 1920;   // TODO(캔버스 정책): 첫 비디오 해상도 추종으로 개선
constexpr int kCanvasHeight = 1080;
constexpr int kCanvasFps = 60;
constexpr GstClockTime kPrerollTimeout = 15 * GST_SECOND;

// 오디오 믹서 고정 출력 포맷 — audiomixer는 모든 sink 패드가 동일 포맷이어야 하므로
// 무음 앵커와 모든 덱 브랜치를 이 포맷으로 핀 고정한다 (Phase 3 멀티채널에서
// channels/channel-mask가 출력 정책값으로 바뀌는 지점).
GstCaps* MakeMixerCaps() {
  return gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "rate",
                             G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2, "layout",
                             G_TYPE_STRING, "interleaved", nullptr);
}

GstElement* MakeElement(const char* factory, const char* name) {
  return gst_element_factory_make(factory, name);
}

std::optional<std::string> ToUri(const std::string& utf8_path) {
  GError* err = nullptr;
  gchar* uri = gst_filename_to_uri(utf8_path.c_str(), &err);
  if (!uri) {
    if (err) g_error_free(err);
    return std::nullopt;
  }
  std::string out(uri);
  g_free(uri);
  return out;
}

// GLib 메인루프로 클로저 마샬링
void InvokeOnMain(std::function<void()> fn) {
  auto* holder = new std::function<void()>(std::move(fn));
  g_main_context_invoke(
      nullptr,
      [](gpointer data) -> gboolean {
        auto* f = static_cast<std::function<void()>*>(data);
        (*f)();
        delete f;
        return G_SOURCE_REMOVE;
      },
      holder);
}

}  // namespace

// ---------------------------------------------------------------------------
// Deck
// ---------------------------------------------------------------------------

struct PlayerCore::Deck {
  PlayerCore* core = nullptr;
  int id = 0;
  json file;
  int track_idx = -1;
  bool play_when_ready = false;

  GstElement* bin = nullptr;
  GstElement* decode = nullptr;  // uridecodebin3

  // 브랜치 끝(마지막 변환 요소)의 src 패드.
  // 프리롤/일시정지 = 이 패드에 BLOCK 프로브 (fakesink 불필요 — FLUSHING으로 인한
  // 태스크 정지 문제를 원천 회피). 스왑 = 고스트 패드로 comp/amix에 연결 후 블록 해제.
  GstElement* video_tail = nullptr;
  GstPad* video_out = nullptr;   // bin 내부 src 패드 (오프셋/시크/쿼리/블록용)
  GstPad* video_ghost = nullptr; // bin 경계 고스트 패드 (bin이 소유)
  gulong video_block = 0;        // 활성 BLOCK 프로브 id (0 = 없음)
  std::atomic<bool> video_ready{false};  // 첫 버퍼 도착(프리롤 완료) — 스트리밍 스레드가 set
  GstElement* audio_tail = nullptr;
  GstPad* audio_out = nullptr;
  GstPad* audio_ghost = nullptr;
  gulong audio_block = 0;
  std::atomic<bool> audio_ready{false};

  GstPad* comp_pad = nullptr;  // 라이브 시 요청 패드 (소유)
  GstPad* amix_pad = nullptr;

  enum class State { Building, Prerolled, Live, Dead };
  State state = State::Building;
  bool eos_sent = false;
  guint preroll_watch = 0;
  GstClockTime preroll_started = 0;
  GstClockTime paused_running = GST_CLOCK_TIME_NONE;  // Pause 시점 러닝타임 (재개 보정용)

  // 이미지 스틸: imagefreeze는 EOS를 내지 않으므로 표시 시간은 타이머가 담당
  bool is_image = false;
  gint64 image_time_ms = 0;                // 0 = 무한 표시
  guint image_timer = 0;                   // g_timeout 소스 id
  GstClockTime image_started = 0;          // 라이브 시작(또는 재개) 시점 러닝타임
  gint64 image_elapsed_ms = 0;             // 일시정지 누적 경과
};

// ---------------------------------------------------------------------------
// 초기화 / 종료
// ---------------------------------------------------------------------------

PlayerCore::PlayerCore() = default;
PlayerCore::~PlayerCore() { Shutdown(); }

bool PlayerCore::Init(HWND video_hwnd, FeedbackFn feedback) {
  hwnd_ = video_hwnd;
  feedback_ = std::move(feedback);

  pipeline_ = gst_pipeline_new("vplayer");

  comp_ = MakeElement("d3d11compositor", "comp");
  if (!comp_) {
    comp_ = MakeElement("compositor", "comp");
    use_d3d11_ = false;
    feedback_("warn", "d3d11compositor unavailable — falling back to software compositor");
  }
  GstElement* vsink = MakeElement(use_d3d11_ ? "d3d11videosink" : "autovideosink", "vsink");
  if (!comp_ || !vsink) {
    feedback_("error", "video output elements unavailable");
    return false;
  }

  amix_ = MakeElement("audiomixer", "amix");
  GstElement* aconv = MakeElement("audioconvert", "aconv_out");
  GstElement* ares = MakeElement("audioresample", "ares_out");
  audio_sink_ = MakeElement("wasapi2sink", "asink");
  if (!audio_sink_) {
    audio_sink_ = MakeElement("autoaudiosink", "asink");
    feedback_("warn", "wasapi2sink unavailable — falling back to autoaudiosink");
  }

  // 배경색 브랜치 (라이브 소스 = 덱이 없어도 컴포지터가 항상 출력)
  bg_src_ = MakeElement("videotestsrc", "bg");
  g_object_set(bg_src_, "pattern", 17 /* solid-color */, "is-live", TRUE,
               "foreground-color", (guint)0xFF000000, nullptr);
  GstElement* bg_caps = MakeElement("capsfilter", "bg_caps");
  {
    GstCaps* caps = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, kCanvasWidth,
                                        "height", G_TYPE_INT, kCanvasHeight, "framerate",
                                        GST_TYPE_FRACTION, kCanvasFps, 1, nullptr);
    g_object_set(bg_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }
  GstElement* bg_upload = use_d3d11_ ? MakeElement("d3d11upload", "bg_upload") : nullptr;

  // 무음 앵커 (덱이 없어도 오디오 클록/믹서 유지)
  GstElement* silence = MakeElement("audiotestsrc", "silence");
  g_object_set(silence, "wave", 4 /* silence */, "is-live", TRUE, nullptr);
  GstElement* silence_caps = MakeElement("capsfilter", "silence_caps");
  {
    GstCaps* caps = MakeMixerCaps();
    g_object_set(silence_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }
  GstElement* silence_conv = MakeElement("audioconvert", "silence_conv");

  gst_bin_add_many(GST_BIN(pipeline_), comp_, vsink, amix_, aconv, ares, audio_sink_, bg_src_,
                   bg_caps, silence, silence_conv, silence_caps, nullptr);
  if (bg_upload) gst_bin_add(GST_BIN(pipeline_), bg_upload);

  if (!gst_element_link(comp_, vsink) || !gst_element_link_many(amix_, aconv, ares, audio_sink_, nullptr)) {
    feedback_("error", "failed to link output stage");
    return false;
  }

  // bg → comp (zorder 0)
  {
    bool ok = bg_upload ? gst_element_link_many(bg_src_, bg_caps, bg_upload, nullptr)
                        : gst_element_link(bg_src_, bg_caps);
    GstElement* bg_end = bg_upload ? bg_upload : bg_caps;
    GstPad* src = gst_element_get_static_pad(bg_end, "src");
    GstPad* sink = gst_element_request_pad_simple(comp_, "sink_%u");
    ok = ok && gst_pad_link(src, sink) == GST_PAD_LINK_OK;
    g_object_set(sink, "zorder", (guint)0, nullptr);
    gst_object_unref(src);
    gst_object_unref(sink);  // 요청 패드지만 파이프라인 수명과 같이 감 — 해제 불필요
    if (!ok) {
      feedback_("error", "failed to link background branch");
      return false;
    }
  }
  // silence → amix
  {
    gst_element_link_many(silence, silence_conv, silence_caps, nullptr);
    GstPad* src = gst_element_get_static_pad(silence_caps, "src");
    GstPad* sink = gst_element_request_pad_simple(amix_, "sink_%u");
    bool ok = gst_pad_link(src, sink) == GST_PAD_LINK_OK;
    gst_object_unref(src);
    gst_object_unref(sink);
    if (!ok) {
      feedback_("error", "failed to link silence branch");
      return false;
    }
  }

  if (!InitLogoBranch()) return false;

  GstBus* bus = gst_element_get_bus(pipeline_);
  gst_bus_set_sync_handler(bus, OnBusSync, this, nullptr);
  gst_bus_add_watch(bus, OnBusMessage, this);
  gst_object_unref(bus);

  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    feedback_("error", "output stage failed to start");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 로고 오버레이 — appsrc에 RGBA 버퍼 1장 push, videoaggregator가 마지막 프레임을
// 유지하므로 정적 로고는 push 1회로 충분. 교체 = caps 변경 후 새 버퍼 push.
// ---------------------------------------------------------------------------

bool PlayerCore::InitLogoBranch() {
  logo_src_ = MakeElement("appsrc", "logo_src");
  // PTS는 push 시 수동 지정 (do-timestamp는 기동 전 push 버퍼에 무효 타임스탬프).
  // max-latency=-1 필수: 기본값 0이면 컴포지터 레이턴시 협상이 bg(live, min 16.7ms)와
  // 모순(max 0 < min)이 되어 CORE/CLOCK 경고가 폭주함 (aggregator 레이턴시 실측 확인)
  g_object_set(logo_src_, "is-live", TRUE, "do-timestamp", FALSE, "format", GST_FORMAT_TIME,
               "min-latency", (gint64)0, "max-latency", (gint64)-1, nullptr);
  GstElement* logo_queue = MakeElement("queue", "logo_queue");
  GstElement* upload = use_d3d11_ ? MakeElement("d3d11upload", "logo_upload") : nullptr;

  gst_bin_add_many(GST_BIN(pipeline_), logo_src_, logo_queue, nullptr);
  if (upload) gst_bin_add(GST_BIN(pipeline_), upload);

  GstElement* tail = upload ? upload : logo_queue;
  bool link_ok = gst_element_link(logo_src_, logo_queue);
  if (upload) link_ok = link_ok && gst_element_link(logo_queue, upload);
  if (!link_ok) {
    feedback_("error", "logo: link failed");
    return false;
  }
  GstPad* src = gst_element_get_static_pad(tail, "src");
  logo_pad_ = gst_element_request_pad_simple(comp_, "sink_%u");
  g_object_set(logo_pad_, "zorder", (guint)100, "alpha", 0.0, nullptr);
  const bool ok = gst_pad_link(src, logo_pad_) == GST_PAD_LINK_OK;
  gst_object_unref(src);
  if (!ok) {
    feedback_("error", "logo: link to compositor failed");
    return false;
  }

  // 초기 1x1 투명 버퍼 (컴포지터 패드 협상용)
  const uint8_t transparent[4] = {0, 0, 0, 0};
  logo_img_w_ = 1;
  logo_img_h_ = 1;
  PushLogoBuffer(1, 1, transparent);
  return true;
}

void PlayerCore::PushLogoBuffer(int w, int h, const uint8_t* rgba) {
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                      G_TYPE_INT, w, "height", G_TYPE_INT, h, "framerate",
                                      GST_TYPE_FRACTION, 0, 1, nullptr);
  gst_app_src_set_caps(GST_APP_SRC(logo_src_), caps);
  gst_caps_unref(caps);

  const size_t size = static_cast<size_t>(w) * h * 4;
  GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
  gst_buffer_fill(buf, 0, rgba, size);

  // PTS = 현재 러닝타임 (기동 전이면 0) — 컴포지터는 패드별 마지막 버퍼를 유지하므로
  // 순서만 맞으면 됨
  GstState state = GST_STATE_NULL;
  gst_element_get_state(pipeline_, &state, nullptr, 0);
  GST_BUFFER_PTS(buf) = (state == GST_STATE_PLAYING) ? RunningTime() : 0;
  GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;

  gst_app_src_push_buffer(GST_APP_SRC(logo_src_), buf);  // 소유권 이전
}

void PlayerCore::ApplyLogoGeometry() {
  if (!logo_pad_ || logo_img_w_ <= 0 || logo_img_h_ <= 0) return;
  int disp_w = logo_size_px_ > 0 ? logo_size_px_ : logo_img_w_;
  int disp_h = disp_w * logo_img_h_ / logo_img_w_;
  if (disp_w > kCanvasWidth) {
    disp_w = kCanvasWidth;
    disp_h = disp_w * logo_img_h_ / logo_img_w_;
  }
  if (disp_h > kCanvasHeight) {
    disp_h = kCanvasHeight;
    disp_w = disp_h * logo_img_w_ / logo_img_h_;
  }
  g_object_set(logo_pad_, "xpos", (kCanvasWidth - disp_w) / 2, "ypos",
               (kCanvasHeight - disp_h) / 2, "width", disp_w, "height", disp_h, nullptr);
}

void PlayerCore::UpdateLogoVisibility(bool emit_feedback) {
  const bool visible = logo_enabled_ && media_wants_logo_ && logo_loaded_;
  if (logo_pad_) g_object_set(logo_pad_, "alpha", visible ? 1.0 : 0.0, nullptr);
  if (emit_feedback) feedback_("logo_visibility", json{{"show", visible}});
}

void PlayerCore::SetLogoFile(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (auto& c : ext) c = static_cast<char>(tolower(c));

  if (ext == ".svg") {
    auto document = lunasvg::Document::loadFromFile(path);
    if (!document) {
      feedback_("error", "logo: failed to load svg: " + path);
      return;
    }
    auto bitmap = document->renderToBitmap();  // 원본 크기, GPU가 표시 크기로 스케일
    if (!bitmap.valid()) {
      feedback_("error", "logo: failed to render svg: " + path);
      return;
    }
    bitmap.convertToRGBA();
    logo_img_w_ = static_cast<int>(bitmap.width());
    logo_img_h_ = static_cast<int>(bitmap.height());
    PushLogoBuffer(logo_img_w_, logo_img_h_, bitmap.data());
  } else {
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!pixels) {
      feedback_("error", "logo: failed to load image: " + path);
      return;
    }
    logo_img_w_ = w;
    logo_img_h_ = h;
    PushLogoBuffer(w, h, pixels);
    stbi_image_free(pixels);
  }

  logo_loaded_ = true;
  ApplyLogoGeometry();
  UpdateLogoVisibility(/*emit_feedback=*/false);
  feedback_("debug", "logo loaded: " + path);
}

void PlayerCore::SetLogoSize(int width_px) {
  logo_size_px_ = width_px;
  ApplyLogoGeometry();
}

void PlayerCore::SetLogoEnabled(bool show) {
  logo_enabled_ = show;
  UpdateLogoVisibility(/*emit_feedback=*/true);
}

void PlayerCore::Shutdown() {
  if (!pipeline_) return;
  for (auto& d : decks_) {
    if (d) {
      if (d->preroll_watch) g_source_remove(d->preroll_watch);
      d.reset();
    }
  }
  gst_element_set_state(pipeline_, GST_STATE_NULL);
  gst_object_unref(pipeline_);
  pipeline_ = nullptr;
}

// ---------------------------------------------------------------------------
// 버스
// ---------------------------------------------------------------------------

GstBusSyncReply PlayerCore::OnBusSync(GstBus*, GstMessage* msg, gpointer user_data) {
  auto* self = static_cast<PlayerCore*>(user_data);
  if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)),
                                        reinterpret_cast<guintptr>(self->hwnd_));
    gst_message_unref(msg);
    return GST_BUS_DROP;
  }
  return GST_BUS_PASS;
}

gboolean PlayerCore::OnBusMessage(GstBus*, GstMessage* msg, gpointer user_data) {
  auto* self = static_cast<PlayerCore*>(user_data);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      std::string src = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
      self->feedback_("error", "pipeline error from " + src + ": " +
                                   (err ? err->message : "unknown"));
      if (err) g_error_free(err);
      g_free(dbg);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      self->feedback_("warn", std::string(err ? err->message : "pipeline warning"));
      if (err) g_error_free(err);
      g_free(dbg);
      break;
    }
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// 덱 빌드 / 프리롤
// ---------------------------------------------------------------------------

void PlayerCore::OnDecodePadAdded(GstElement*, GstPad* pad, gpointer user_data) {
  // 스트리밍 스레드에서 호출됨 — bin 내부 구성만 하고 상태는 bin(PAUSED 잠금)에 동기화
  auto* deck = static_cast<Deck*>(user_data);
  PlayerCore* core = deck->core;

  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  const bool is_video = g_str_has_prefix(name, "video/");
  const bool is_audio = g_str_has_prefix(name, "audio/");
  gst_caps_unref(caps);

  if (is_video && !deck->video_tail) {
    GstElement* q = MakeElement("queue", nullptr);
    // 이미지: 단일 프레임을 무한 반복 스트림으로 (표시 시간은 SwapTo의 타이머가 관리)
    GstElement* freeze = deck->is_image ? MakeElement("imagefreeze", nullptr) : nullptr;
    GstElement* freeze_caps = nullptr;
    if (freeze) {
      freeze_caps = MakeElement("capsfilter", nullptr);
      GstCaps* caps = gst_caps_new_simple("video/x-raw", "framerate", GST_TYPE_FRACTION, 30, 1,
                                          nullptr);
      g_object_set(freeze_caps, "caps", caps, nullptr);
      gst_caps_unref(caps);
    }
    GstElement* upload = core->use_d3d11_ ? MakeElement("d3d11upload", nullptr) : nullptr;
    GstElement* convert = core->use_d3d11_ ? MakeElement("d3d11convert", nullptr)
                                           : MakeElement("videoconvert", nullptr);

    gst_bin_add_many(GST_BIN(deck->bin), q, convert, nullptr);
    if (freeze) gst_bin_add_many(GST_BIN(deck->bin), freeze, freeze_caps, nullptr);
    if (upload) gst_bin_add(GST_BIN(deck->bin), upload);

    bool ok = true;
    GstElement* chain_head = q;
    if (freeze) {
      ok = ok && gst_element_link_many(q, freeze, freeze_caps, nullptr);
      chain_head = freeze_caps;
    }
    ok = ok && (upload ? gst_element_link_many(chain_head, upload, convert, nullptr)
                       : gst_element_link(chain_head, convert));
    GstPad* qsink = gst_element_get_static_pad(q, "sink");
    ok = ok && gst_pad_link(pad, qsink) == GST_PAD_LINK_OK;
    gst_object_unref(qsink);

    deck->video_tail = convert;
    deck->video_out = gst_element_get_static_pad(convert, "src");

    // 첫 버퍼를 tail에서 블록 = 프리롤 완료 신호 (버퍼만 블록, 이벤트는 통과)
    deck->video_block = gst_pad_add_probe(
        deck->video_out,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
        [](GstPad*, GstPadProbeInfo*, gpointer user_data) -> GstPadProbeReturn {
          static_cast<Deck*>(user_data)->video_ready = true;
          return GST_PAD_PROBE_OK;  // 블록 유지 — 스왑에서 해제
        },
        deck, nullptr);

    gst_element_sync_state_with_parent(q);
    if (freeze) {
      gst_element_sync_state_with_parent(freeze);
      gst_element_sync_state_with_parent(freeze_caps);
    }
    if (upload) gst_element_sync_state_with_parent(upload);
    gst_element_sync_state_with_parent(convert);
    if (!ok) InvokeOnMain([core] { core->feedback_("error", "deck: video branch link failed"); });

    // EOS 감지 (스트리밍 스레드 → 메인 마샬링)
    gst_pad_add_probe(
        deck->video_out, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        [](GstPad*, GstPadProbeInfo* info, gpointer user_data) -> GstPadProbeReturn {
          auto* d = static_cast<Deck*>(user_data);
          if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS &&
              d->state == Deck::State::Live && !d->eos_sent) {
            d->eos_sent = true;
            PlayerCore* c = d->core;
            const int track = d->track_idx;
            const int id = d->id;
            InvokeOnMain([c, track, id] {
              c->feedback_("end_reached",
                           json{{"playlist_track_index", track}, {"active_player_id", id}});
            });
          }
          return GST_PAD_PROBE_OK;
        },
        deck, nullptr);
  } else if (is_audio && !deck->audio_tail) {
    GstElement* q = MakeElement("queue", nullptr);
    GstElement* conv = MakeElement("audioconvert", nullptr);
    GstElement* res = MakeElement("audioresample", nullptr);
    // 믹서 고정 포맷으로 핀 — 프리롤 단계에서 바로 amix 호환 caps로 협상시켜
    // 스왑 시 not-negotiated를 방지 (audiomixer는 패드별 변환이 없음)
    GstElement* capsf = MakeElement("capsfilter", nullptr);
    {
      GstCaps* caps = MakeMixerCaps();
      g_object_set(capsf, "caps", caps, nullptr);
      gst_caps_unref(caps);
    }
    GstElement* vol = MakeElement("volume", nullptr);

    gst_bin_add_many(GST_BIN(deck->bin), q, conv, res, capsf, vol, nullptr);
    bool ok = gst_element_link_many(q, conv, res, capsf, vol, nullptr);
    GstPad* qsink = gst_element_get_static_pad(q, "sink");
    ok = ok && gst_pad_link(pad, qsink) == GST_PAD_LINK_OK;
    gst_object_unref(qsink);

    deck->audio_tail = vol;
    deck->audio_out = gst_element_get_static_pad(vol, "src");

    deck->audio_block = gst_pad_add_probe(
        deck->audio_out,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
        [](GstPad*, GstPadProbeInfo*, gpointer user_data) -> GstPadProbeReturn {
          static_cast<Deck*>(user_data)->audio_ready = true;
          return GST_PAD_PROBE_OK;
        },
        deck, nullptr);

    gst_element_sync_state_with_parent(q);
    gst_element_sync_state_with_parent(conv);
    gst_element_sync_state_with_parent(res);
    gst_element_sync_state_with_parent(capsf);
    gst_element_sync_state_with_parent(vol);
    if (!ok) InvokeOnMain([core] { core->feedback_("error", "deck: audio branch link failed"); });
  }
  // 자막 등 기타 스트림은 무시 (연결 안 된 패드는 decodebin3가 처리)
}

PlayerCore::Deck* PlayerCore::BuildDeck(int deck_id, const json& file, int track_idx,
                                        bool play_when_ready, double image_time_s) {
  const std::string path = file.value("path", "");
  auto uri = ToUri(path);
  if (!uri) {
    feedback_("error", "invalid media path: " + path);
    return nullptr;
  }

  if (decks_[deck_id]) TeardownDeck(decks_[deck_id].get());

  auto deck = std::make_unique<Deck>();
  deck->core = this;
  deck->id = deck_id;
  deck->file = file;
  deck->track_idx = track_idx;
  deck->play_when_ready = play_when_ready;
  // is_image 판정: 명시 플래그 우선, 없으면 mimetype (프로토콜 §file 객체의 누락 모순은
  // 호스트 측에서 정리하기로 함 — 여기서는 명시값만 신뢰)
  deck->is_image = file.value("is_image", false) ||
                   file.value("mimetype", std::string()).rfind("image/", 0) == 0;
  deck->image_time_ms = static_cast<gint64>(image_time_s * 1000.0);

  deck->bin = gst_bin_new(deck_id == 0 ? "deck0" : "deck1");
  deck->decode = MakeElement("uridecodebin3", nullptr);
  g_object_set(deck->decode, "uri", uri->c_str(), nullptr);
  g_signal_connect(deck->decode, "pad-added", G_CALLBACK(OnDecodePadAdded), deck.get());
  gst_bin_add(GST_BIN(deck->bin), deck->decode);

  gst_bin_add(GST_BIN(pipeline_), deck->bin);
  gst_element_set_locked_state(deck->bin, TRUE);
  gst_element_set_state(deck->bin, GST_STATE_PAUSED);

  deck->preroll_started = gst_clock_get_time(gst_system_clock_obtain());
  Deck* raw = deck.get();
  deck->preroll_watch = g_timeout_add(50, [](gpointer data) -> gboolean {
    auto* d = static_cast<Deck*>(data);
    if (d->core->CheckPreroll(d)) return G_SOURCE_CONTINUE;
    d->preroll_watch = 0;
    return G_SOURCE_REMOVE;
  }, raw);

  decks_[deck_id] = std::move(deck);
  return raw;
}

bool PlayerCore::CheckPreroll(Deck* deck) {
  GstState state = GST_STATE_NULL;
  if (gst_element_get_state(deck->bin, &state, nullptr, 0) == GST_STATE_CHANGE_FAILURE) {
    feedback_("error", "deck preroll failed: " + deck->file.value("path", ""));
    TeardownDeck(deck);
    return false;
  }

  // 프리롤 완료 = 존재하는 모든 브랜치의 tail에 첫 버퍼가 블록됨.
  // decodebin3는 스트림 컬렉션 시점에 모든 패드를 함께 노출하므로, 버퍼가 도착할
  // 즈음이면 브랜치 구성은 끝나 있음 — 300ms 그레이스는 예외적 디먹서 대비.
  const GstClockTime now = gst_clock_get_time(gst_system_clock_obtain());
  const bool has_branch = deck->video_tail || deck->audio_tail;
  const bool video_ok = !deck->video_tail || deck->video_ready;
  const bool audio_ok = !deck->audio_tail || deck->audio_ready;
  if (has_branch && video_ok && audio_ok && now - deck->preroll_started > 300 * GST_MSECOND) {
    deck->state = Deck::State::Prerolled;
    if (deck->play_when_ready) {
      SwapTo(deck);
    } else {
      standby_deck_ = deck->id;
      feedback_("debug", "deck " + std::to_string(deck->id) + " preloaded");
    }
    return false;  // 폴링 종료
  }

  if (now - deck->preroll_started > kPrerollTimeout) {
    feedback_("error", "deck preroll timeout: " + deck->file.value("path", ""));
    TeardownDeck(deck);
    return false;
  }
  return true;  // 계속 폴링
}

// ---------------------------------------------------------------------------
// 스왑 / 해체
// ---------------------------------------------------------------------------

GstClockTime PlayerCore::RunningTime() const {
  GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(pipeline_));
  const GstClockTime now = gst_clock_get_time(clock);
  gst_object_unref(clock);
  return now - gst_element_get_base_time(pipeline_);
}

void PlayerCore::SwapTo(Deck* deck) {
  const GstClockTime offset = RunningTime();

  // 비디오 브랜치: 고스트 패드로 bin 경계 넘어 comp 요청 패드에 연결 (tail은 블록 상태)
  if (deck->video_out) {
    deck->video_ghost = gst_ghost_pad_new("video_src", deck->video_out);
    gst_pad_set_active(deck->video_ghost, TRUE);
    gst_element_add_pad(deck->bin, deck->video_ghost);

    deck->comp_pad = gst_element_request_pad_simple(comp_, "sink_%u");
    g_object_set(deck->comp_pad, "zorder", (guint)(1 + deck->id), "alpha", 1.0, "xpos", 0,
                 "ypos", 0, "width", kCanvasWidth, "height", kCanvasHeight, nullptr);
    // sizing-policy: keep-aspect-ratio (enum=1) — 캔버스에 레터박스로 맞춤
    g_object_set(deck->comp_pad, "sizing-policy", 1, nullptr);
    gst_pad_set_offset(deck->video_out, static_cast<gint64>(offset));
    if (gst_pad_link(deck->video_ghost, deck->comp_pad) != GST_PAD_LINK_OK) {
      feedback_("error", "swap: video link to compositor failed");
    }
  }
  // 오디오 브랜치: 고스트 패드로 amix 요청 패드에 연결
  if (deck->audio_out) {
    deck->audio_ghost = gst_ghost_pad_new("audio_src", deck->audio_out);
    gst_pad_set_active(deck->audio_ghost, TRUE);
    gst_element_add_pad(deck->bin, deck->audio_ghost);

    deck->amix_pad = gst_element_request_pad_simple(amix_, "sink_%u");
    gst_pad_set_offset(deck->audio_out, static_cast<gint64>(offset));
    if (gst_pad_link(deck->audio_ghost, deck->amix_pad) != GST_PAD_LINK_OK) {
      feedback_("error", "swap: audio link to mixer failed");
    }
  }

  gst_element_set_locked_state(deck->bin, FALSE);
  gst_element_sync_state_with_parent(deck->bin);

  // 연결 완료 후 블록 해제 → 첫 버퍼부터 comp/amix로 흐름
  if (deck->video_block) {
    gst_pad_remove_probe(deck->video_out, deck->video_block);
    deck->video_block = 0;
  }
  if (deck->audio_block) {
    gst_pad_remove_probe(deck->audio_out, deck->audio_block);
    deck->audio_block = 0;
  }
  deck->state = Deck::State::Live;
  paused_ = false;

  // 이전 라이브 덱: 즉시 숨김(하드 컷) 후 지연 해체
  if (live_deck_ >= 0 && live_deck_ != deck->id && decks_[live_deck_]) {
    Deck* old = decks_[live_deck_].get();
    if (old->comp_pad) g_object_set(old->comp_pad, "alpha", 0.0, nullptr);
    if (old->amix_pad) g_object_set(old->amix_pad, "mute", TRUE, nullptr);
    PlayerCore* self = this;
    const int old_id = old->id;
    g_timeout_add(100, [](gpointer data) -> gboolean {
      auto* p = static_cast<std::pair<PlayerCore*, int>*>(data);
      if (p->first->decks_[p->second] &&
          p->first->decks_[p->second]->state == Deck::State::Live) {
        // 이미 새 덱이 이 슬롯을 차지한 경우는 건너뜀
      }
      if (p->first->decks_[p->second] && p->first->live_deck_ != p->second) {
        p->first->TeardownDeck(p->first->decks_[p->second].get());
      }
      delete p;
      return G_SOURCE_REMOVE;
    }, new std::pair<PlayerCore*, int>(self, old_id));
  }

  live_deck_ = deck->id;
  if (standby_deck_ == deck->id) standby_deck_ = -1;

  // 이미지 표시 시간 타이머 (0 = 무한)
  deck->image_started = offset;
  deck->image_elapsed_ms = 0;
  if (deck->is_image && deck->image_time_ms > 0) {
    deck->image_timer = g_timeout_add(
        static_cast<guint>(deck->image_time_ms),
        [](gpointer data) -> gboolean {
          auto* d = static_cast<Deck*>(data);
          d->image_timer = 0;
          if (d->state == Deck::State::Live && !d->eos_sent) {
            d->eos_sent = true;
            d->core->feedback_("end_reached", json{{"playlist_track_index", d->track_idx},
                                                   {"active_player_id", d->id}});
          }
          return G_SOURCE_REMOVE;
        },
        deck);
  }

  // 로고 자동 표시 규칙: 이미지/비디오 = 숨김, 오디오 전용 = 표시 (§2.7)
  media_wants_logo_ = (deck->video_tail == nullptr);
  UpdateLogoVisibility(/*emit_feedback=*/true);

  feedback_("active_player_id", deck->id);
  json changed = {{"idx", deck->id}};
  if (deck->file.contains("uuid")) changed["uuid"] = deck->file["uuid"];
  if (deck->file.contains("path")) changed["path"] = deck->file["path"];
  if (deck->track_idx >= 0) changed["playlist_track_index"] = deck->track_idx;
  feedback_("media_changed", changed);
}

void PlayerCore::TeardownDeck(Deck* deck) {
  const int id = deck->id;
  if (deck->preroll_watch) {
    g_source_remove(deck->preroll_watch);
    deck->preroll_watch = 0;
  }
  if (deck->image_timer) {
    g_source_remove(deck->image_timer);
    deck->image_timer = 0;
  }
  deck->state = Deck::State::Dead;

  if (deck->video_block) {
    gst_pad_remove_probe(deck->video_out, deck->video_block);
    deck->video_block = 0;
  }
  if (deck->audio_block) {
    gst_pad_remove_probe(deck->audio_out, deck->audio_block);
    deck->audio_block = 0;
  }

  gst_element_set_locked_state(deck->bin, TRUE);
  gst_element_set_state(deck->bin, GST_STATE_NULL);

  if (deck->comp_pad) {
    if (deck->video_ghost) gst_pad_unlink(deck->video_ghost, deck->comp_pad);
    gst_element_release_request_pad(comp_, deck->comp_pad);
    gst_object_unref(deck->comp_pad);
  }
  if (deck->amix_pad) {
    if (deck->audio_ghost) gst_pad_unlink(deck->audio_ghost, deck->amix_pad);
    gst_element_release_request_pad(amix_, deck->amix_pad);
    gst_object_unref(deck->amix_pad);
  }
  if (deck->video_out) gst_object_unref(deck->video_out);
  if (deck->audio_out) gst_object_unref(deck->audio_out);

  gst_bin_remove(GST_BIN(pipeline_), deck->bin);  // bin unref 포함

  if (live_deck_ == id) live_deck_ = -1;
  if (standby_deck_ == id) standby_deck_ = -1;
  decks_[id].reset();
}

// ---------------------------------------------------------------------------
// 공개 명령
// ---------------------------------------------------------------------------

void PlayerCore::PlayFile(const json& file, int track_idx, double image_time_s) {
  // 라이브 덱과 다른 슬롯에 빌드 → 프리롤 완료 시 자동 스왑
  const int slot = (live_deck_ == 0) ? 1 : 0;
  if (standby_deck_ >= 0 && decks_[standby_deck_]) TeardownDeck(decks_[standby_deck_].get());
  BuildDeck(slot, file, track_idx, /*play_when_ready=*/true, image_time_s);
}

void PlayerCore::PreloadNext(const json& file, int track_idx, double image_time_s) {
  const int slot = (live_deck_ == 0) ? 1 : 0;
  BuildDeck(slot, file, track_idx, /*play_when_ready=*/false, image_time_s);
}

bool PlayerCore::Next() {
  if (standby_deck_ < 0 || !decks_[standby_deck_] ||
      decks_[standby_deck_]->state != Deck::State::Prerolled) {
    feedback_("warn", "next: no preloaded deck");
    return false;
  }
  SwapTo(decks_[standby_deck_].get());
  return true;
}

void PlayerCore::Play() {
  if (live_deck_ < 0 || !decks_[live_deck_]) return;
  Deck* deck = decks_[live_deck_].get();
  if (!paused_) return;

  // 일시정지 동안 흐른 러닝타임만큼 오프셋 보정 후 블록 해제
  const GstClockTime delta = RunningTime() - deck->paused_running;
  if (deck->video_out)
    gst_pad_set_offset(deck->video_out, gst_pad_get_offset(deck->video_out) + delta);
  if (deck->audio_out)
    gst_pad_set_offset(deck->audio_out, gst_pad_get_offset(deck->audio_out) + delta);

  if (deck->video_block) {
    gst_pad_remove_probe(deck->video_out, deck->video_block);
    deck->video_block = 0;
  }
  if (deck->audio_block) {
    gst_pad_remove_probe(deck->audio_out, deck->audio_block);
    deck->audio_block = 0;
  }

  // 이미지 타이머 재개 (남은 시간만큼 재무장)
  if (deck->is_image && deck->image_time_ms > 0 && !deck->eos_sent) {
    deck->image_started = RunningTime();
    const gint64 remain = deck->image_time_ms - deck->image_elapsed_ms;
    if (remain > 0 && !deck->image_timer) {
      deck->image_timer = g_timeout_add(
          static_cast<guint>(remain),
          [](gpointer data) -> gboolean {
            auto* d = static_cast<Deck*>(data);
            d->image_timer = 0;
            if (d->state == Deck::State::Live && !d->eos_sent) {
              d->eos_sent = true;
              d->core->feedback_("end_reached", json{{"playlist_track_index", d->track_idx},
                                                     {"active_player_id", d->id}});
            }
            return G_SOURCE_REMOVE;
          },
          deck);
    }
  }
  paused_ = false;
}

void PlayerCore::Pause() {
  // 싱크 없는 서브 bin은 PAUSED로 바꿔도 흐름이 계속되므로(실측 확인),
  // 상태 변경 대신 tail 패드 BLOCK으로 일시정지를 구현한다.
  // 컴포지터는 마지막 프레임을 유지(정지 화면), 믹서는 해당 패드만 무음 처리.
  if (live_deck_ < 0 || !decks_[live_deck_] || paused_) return;
  Deck* deck = decks_[live_deck_].get();
  deck->paused_running = RunningTime();
  if (deck->video_out && !deck->video_block) {
    deck->video_block = gst_pad_add_probe(
        deck->video_out,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
        nullptr, nullptr, nullptr);
  }
  if (deck->audio_out && !deck->audio_block) {
    deck->audio_block = gst_pad_add_probe(
        deck->audio_out,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
        nullptr, nullptr, nullptr);
  }

  // 이미지 타이머 일시정지 (경과 누적 후 해제)
  if (deck->is_image) {
    deck->image_elapsed_ms +=
        static_cast<gint64>((RunningTime() - deck->image_started) / GST_MSECOND);
    if (deck->image_timer) {
      g_source_remove(deck->image_timer);
      deck->image_timer = 0;
    }
  }
  paused_ = true;
}

void PlayerCore::Stop() {
  if (live_deck_ >= 0 && decks_[live_deck_]) TeardownDeck(decks_[live_deck_].get());
  if (standby_deck_ >= 0 && decks_[standby_deck_]) TeardownDeck(decks_[standby_deck_].get());
  paused_ = false;
  // 정지 → 로고 복귀 (프로토콜 §2.7: stop 후 logo_visibility {show:true} 필수)
  media_wants_logo_ = true;
  UpdateLogoVisibility(/*emit_feedback=*/true);
}

void PlayerCore::SeekMs(int64_t time_ms) {
  if (live_deck_ < 0 || !decks_[live_deck_]) return;
  Deck* deck = decks_[live_deck_].get();
  if (deck->is_image) {
    feedback_("debug", "set_time ignored for image");
    return;
  }
  GstPad* pad = deck->video_out ? deck->video_out : deck->audio_out;
  if (!pad) return;

  GstEvent* seek = gst_event_new_seek(
      1.0, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
      GST_SEEK_TYPE_SET, time_ms * GST_MSECOND, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  gst_pad_send_event(pad, seek);

  // 플러시 후 러닝타임이 0부터 재시작하므로 오프셋 재설정
  const GstClockTime offset = RunningTime();
  if (deck->video_out) gst_pad_set_offset(deck->video_out, static_cast<gint64>(offset));
  if (deck->audio_out) gst_pad_set_offset(deck->audio_out, static_cast<gint64>(offset));
}

void PlayerCore::SetBackgroundColor(uint32_t rgb) {
  if (bg_src_) g_object_set(bg_src_, "foreground-color", (guint)(0xFF000000u | rgb), nullptr);
}

json PlayerCore::ListAudioDevices() {
  json devices = json::array();

  GstDeviceProviderFactory* factory =
      gst_device_provider_factory_find("wasapi2deviceprovider");
  GstDeviceProvider* provider =
      factory ? gst_device_provider_factory_get(factory) : nullptr;
  if (factory) gst_object_unref(factory);

  if (provider) {
    gst_device_provider_start(provider);
    GList* list = gst_device_provider_get_devices(provider);
    for (GList* it = list; it; it = it->next) {
      GstDevice* dev = GST_DEVICE(it->data);
      if (!gst_device_has_classes(dev, "Audio/Sink")) continue;
      gchar* name = gst_device_get_display_name(dev);
      GstStructure* props = gst_device_get_properties(dev);
      const gchar* dev_id = props ? gst_structure_get_string(props, "device.id") : nullptr;
      devices.push_back({{"deviceId", dev_id ? dev_id : (name ? name : "")},
                         {"name", name ? name : ""},
                         {"type", "wasapi"}});
      if (props) gst_structure_free(props);
      g_free(name);
    }
    g_list_free_full(list, gst_object_unref);
    gst_device_provider_stop(provider);
    gst_object_unref(provider);
  }
  // TODO(asio): gstasio.dll 번들 후 asiodeviceprovider 결과를 type:"asio"로 병합
  return devices;
}

void PlayerCore::SetAudioDevice(const std::string& device_id) {
  // wasapi2sink의 device 속성은 READY 이하에서만 반영됨 — 현 단계에서는 저장 후
  // 다음 파이프라인 기동 시 적용. TODO(오디오): AudioOutput 팩토리에서 무중단 전환 구현.
  if (audio_sink_) g_object_set(audio_sink_, "device", device_id.c_str(), nullptr);
  feedback_("info", "audio device stored (applied on next start): " + device_id);
}

void PlayerCore::EmitTick() {
  if (live_deck_ < 0 || !decks_[live_deck_]) return;
  Deck* deck = decks_[live_deck_].get();
  GstPad* pad = deck->video_out ? deck->video_out : deck->audio_out;
  if (!pad) return;

  gint64 time_ms = 0, dur_ms = 0;
  if (deck->is_image) {
    // 이미지: 타이머 기반 합성 (Python player와 동일 — 프로토콜 §3.4)
    time_ms = deck->image_elapsed_ms +
              (paused_ ? 0
                       : static_cast<gint64>((RunningTime() - deck->image_started) / GST_MSECOND));
    dur_ms = deck->image_time_ms;  // 0 = 무한
    if (dur_ms > 0 && time_ms > dur_ms) time_ms = dur_ms;
  } else {
    gint64 pos_ns = -1, dur_ns = -1;
    gst_pad_query_position(pad, GST_FORMAT_TIME, &pos_ns);
    gst_pad_query_duration(pad, GST_FORMAT_TIME, &dur_ns);
    time_ms = pos_ns >= 0 ? pos_ns / GST_MSECOND : 0;
    dur_ms = dur_ns >= 0 ? dur_ns / GST_MSECOND : 0;
  }
  const double position = dur_ms > 0 ? static_cast<double>(time_ms) / dur_ms : 0.0;
  const bool playing = !paused_;

  feedback_("player_data", json{{"id", deck->id},
                                {"event", playing ? "playing" : "paused"},
                                {"time", time_ms},
                                {"duration", dur_ms},
                                {"position", position},
                                {"is_playing", playing},
                                {"state", playing ? "playing" : "paused"}});
}

}  // namespace vp
