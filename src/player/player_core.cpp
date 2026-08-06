#include "player/player_core.h"
#include "player/player_internal.h"

#include <gst/app/gstappsrc.h>
#include <gst/audio/audio.h>
#include <gst/net/net.h>
#include <gst/video/videooverlay.h>

#include <algorithm>
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
constexpr guint kLivenessHealMs = 3000;  // play_synced 후 이 시간까지 클립이 Live 못 되면 자가치유
constexpr size_t kMaxAudioTracks = 8;  // 독립 오디오 트랙 동시 상한 (v2 §5)

// 오디오 버스(믹서 출력) 포맷 — Phase 3 멀티채널: 채널수 N은 출력 디바이스를 따른다
// (wasapi = min(ch,8) positioned fallback mask / asio = 드라이버 보고값 unpositioned).
GstCaps* MakeBusCaps(int channels, bool positioned) {
  GstCaps* caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "rate",
                                      G_TYPE_INT, 48000, "channels", G_TYPE_INT, channels,
                                      "layout", G_TYPE_STRING, "interleaved", nullptr);
  guint64 mask = 0;
  if (positioned && channels <= 8) mask = gst_audio_channel_get_fallback_mask(channels);
  gst_caps_set_simple(caps, "channel-mask", GST_TYPE_BITMASK, mask, nullptr);
  return caps;
}

// 덱/무음 브랜치 고정 caps — audiomixer 패드별 변환이 브랜치 채널수 → 버스 채널수를
// mix-matrix로 배치하므로, 브랜치는 라우팅 폭(channel_map 길이, 기본 2ch)으로만 핀 고정.
GstCaps* MakeBranchCaps(int channels) {
  return gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "rate",
                             G_TYPE_INT, 48000, "channels", G_TYPE_INT, channels, "layout",
                             G_TYPE_STRING, "interleaved", nullptr);
}

// (ChannelRoute 정의는 player_internal.h로 이동 — 두 TU 공유)

// mix-matrix 규약: 행 = 출력(버스) 채널, 열 = 입력(브랜치) 채널.
GstStructure* MakeMatrixConfig(int src_ch, int out_ch, const std::vector<ChannelRoute>& routes) {
  GValue matrix = G_VALUE_INIT;
  g_value_init(&matrix, GST_TYPE_ARRAY);
  for (int o = 0; o < out_ch; ++o) {
    GValue row = G_VALUE_INIT;
    g_value_init(&row, GST_TYPE_ARRAY);
    for (int s = 0; s < src_ch; ++s) {
      float coeff = 0.0f;
      if (s < static_cast<int>(routes.size())) {
        const ChannelRoute& r = routes[s];
        if (!r.muted && r.out == o) coeff = r.gain;
      } else if (routes.empty() && s == o) {
        coeff = 1.0f;  // 빈 routes = 항등
      }
      GValue v = G_VALUE_INIT;
      g_value_init(&v, G_TYPE_FLOAT);
      g_value_set_float(&v, coeff);
      gst_value_array_append_value(&row, &v);
      g_value_unset(&v);
    }
    gst_value_array_append_value(&matrix, &row);
    g_value_unset(&row);
  }
  GstStructure* st = gst_structure_new_empty("converter-config");
  gst_structure_set_value(st, GST_AUDIO_CONVERTER_OPT_MIX_MATRIX, &matrix);
  g_value_unset(&matrix);
  return st;
}

GstStructure* MakeMatrixConfig(int src_ch, int out_ch, const std::vector<int>& map) {
  std::vector<ChannelRoute> routes;
  routes.reserve(map.size());
  for (int out : map) routes.push_back({out, 1.0f, out < 0});
  return MakeMatrixConfig(src_ch, out_ch, routes);
}

void SetPadMatrix(GstPad* pad, int src_ch, int out_ch, const std::vector<int>& map) {
  GstStructure* st = MakeMatrixConfig(src_ch, out_ch, map);
  g_object_set(pad, "converter-config", st, nullptr);
  gst_structure_free(st);
}
void SetPadMatrix(GstPad* pad, int src_ch, int out_ch, const std::vector<ChannelRoute>& routes) {
  GstStructure* st = MakeMatrixConfig(src_ch, out_ch, routes);
  g_object_set(pad, "converter-config", st, nullptr);
  gst_structure_free(st);
}

struct StreamAudio {
  std::vector<ChannelRoute> routes;
  double volume_gain = 1.0;
  bool master_muted = false;
};

ChannelRoute ParseChannel(const nlohmann::json& c) {
  ChannelRoute r;
  r.out = c.value("out", -1);
  r.gain = static_cast<float>(std::clamp(c.value("volume", 100.0), 0.0, 100.0) / 100.0);
  r.muted = c.value("muted", false);
  return r;
}

StreamAudio ParseFileAudio(const nlohmann::json& file) {
  StreamAudio a;
  const nlohmann::json* stream = nullptr;
  if (const auto it = file.find("embedded_streams");
      it != file.end() && it->is_array() && !it->empty()) {
    stream = &(*it)[0];
  }
  if (stream) {
    if (const auto ch = stream->find("channels"); ch != stream->end() && ch->is_array()) {
      for (const auto& c : *ch) a.routes.push_back(ParseChannel(c));
    }
    a.volume_gain = std::clamp(stream->value("volume", 100.0), 0.0, 100.0) / 100.0;
    a.master_muted = stream->value("muted", false);
  } else {
    if (const auto it = file.find("channel_map");
        it != file.end() && it->is_array() && !it->empty()) {
      for (const auto& v : *it) {
        const int out = v.is_number_integer() ? v.get<int>() : -1;
        a.routes.push_back({out, 1.0f, out < 0});
      }
    }
    if (const auto it = file.find("volume"); it != file.end() && it->is_number()) {
      a.volume_gain = std::clamp(it->get<double>(), 0.0, 100.0) / 100.0;
    }
    a.master_muted = file.value("muted", false);
  }
  return a;
}

GstElement* MakeElement(const char* factory, const char* name) {
  return gst_element_factory_make(factory, name);
}
// 창별 요소 이름 충돌 방지: factory + suffix (window_id 등)
GstElement* MakeElementN(const char* factory, const std::string& name) {
  return gst_element_factory_make(factory, name.c_str());
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

// Deck / AudioTrack / Surface 정의는 player_internal.h 참조.

// ---------------------------------------------------------------------------
// 초기화 / 종료
// ---------------------------------------------------------------------------

PlayerCore::PlayerCore() = default;
PlayerCore::~PlayerCore() { Shutdown(); }

PlayerCore::Surface* PlayerCore::GetSurface(int window_id) {
  auto it = surfaces_.find(window_id);
  return it == surfaces_.end() ? nullptr : it->second.get();
}
const PlayerCore::Surface* PlayerCore::GetSurface(int window_id) const {
  auto it = surfaces_.find(window_id);
  return it == surfaces_.end() ? nullptr : it->second.get();
}

bool PlayerCore::Init(FeedbackFn feedback) {
  feedback_ = std::move(feedback);

  pipeline_ = gst_pipeline_new("vplayer");

  // d3d11 가용성 판정 (첫 comp 생성으로) — 전 창 공통 정책.
  // HW 가속을 끈 경우 프로브를 건너뛰고 소프트웨어 렌더 경로로 강제.
  if (hwaccel_enabled_) {
    GstElement* probe = MakeElement("d3d11compositor", nullptr);
    if (probe) {
      use_d3d11_ = true;
      gst_object_unref(probe);
    } else {
      use_d3d11_ = false;
      feedback_("warn", "d3d11compositor unavailable — falling back to software compositor");
    }
  } else {
    use_d3d11_ = false;
    feedback_("info", "hardware acceleration disabled — software render/decode");
  }

  amix_ = MakeElement("audiomixer", "amix");
  bus_caps_ = MakeElement("capsfilter", "bus_caps");
  {
    GstCaps* caps = MakeBusCaps(output_channels_, bus_positioned_);
    g_object_set(bus_caps_, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }
  GstElement* aconv = MakeElement("audioconvert", "aconv_out");
  GstElement* ares = MakeElement("audioresample", "ares_out");
  // 전역 마스터 볼륨 (출력 최종단, 전 소스 합산 후). sink 교체와 무관하게 유지되도록 ares 뒤,
  // sink 앞에 고정. sink 교체는 audio_tail_↔sink를 언링크/재링크하므로 audio_tail_ = master_vol_.
  master_vol_ = MakeElement("volume", "master_vol");
  audio_tail_ = master_vol_;
  if (master_vol_) g_object_set(master_vol_, "volume", master_volume_, nullptr);
  audio_sink_ = MakeElement("wasapi2sink", "asink");
  if (!audio_sink_) {
    audio_sink_ = MakeElement("autoaudiosink", "asink");
    feedback_("warn", "wasapi2sink unavailable — falling back to autoaudiosink");
  }
  if (!amix_ || !bus_caps_ || !aconv || !ares || !master_vol_ || !audio_sink_) {
    feedback_("error", "audio bus elements unavailable");
    return false;
  }

  // 시스템 클록 고정 (멀티 PC PTP 전환의 교체 지점 — Phase 5).
  {
    GstClock* sysclock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(pipeline_), sysclock);
    gst_object_unref(sysclock);
  }

  // 무음 앵커 (덱이 없어도 오디오 클록/믹서 유지)
  GstElement* silence = MakeElement("audiotestsrc", "silence");
  g_object_set(silence, "wave", 4 /* silence */, "is-live", TRUE, nullptr);
  GstElement* silence_caps = MakeElement("capsfilter", "silence_caps");
  {
    GstCaps* caps = MakeBranchCaps(1);
    g_object_set(silence_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }
  GstElement* silence_conv = MakeElement("audioconvert", "silence_conv");

  gst_bin_add_many(GST_BIN(pipeline_), amix_, bus_caps_, aconv, ares, master_vol_, audio_sink_,
                   silence, silence_conv, silence_caps, nullptr);

  if (!gst_element_link_many(amix_, bus_caps_, aconv, ares, master_vol_, audio_sink_, nullptr)) {
    feedback_("error", "failed to link audio output stage");
    return false;
  }
  // 출력 채널별 지연 라인 — bus_caps 출력(Nch F32LE 인터리브)에 in-place 프로브. 기본 패스스루.
  {
    GstPad* bp = gst_element_get_static_pad(bus_caps_, "src");
    gst_pad_add_probe(bp, GST_PAD_PROBE_TYPE_BUFFER, OnBusAudioProbe, this, nullptr);
    gst_object_unref(bp);
  }
  {
    gst_element_link_many(silence, silence_conv, silence_caps, nullptr);
    GstPad* src = gst_element_get_static_pad(silence_caps, "src");
    silence_pad_ = gst_element_request_pad_simple(amix_, "sink_%u");
    bool ok = gst_pad_link(src, silence_pad_) == GST_PAD_LINK_OK;
    gst_object_unref(src);
    if (!ok) {
      feedback_("error", "failed to link silence branch");
      return false;
    }
    SetPadMatrix(silence_pad_, 1, output_channels_, {-1});
  }

  GstBus* bus = gst_element_get_bus(pipeline_);
  gst_bus_set_sync_handler(bus, OnBusSync, this, nullptr);
  gst_bus_add_watch(bus, OnBusMessage, this);
  gst_object_unref(bus);

  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    feedback_("error", "audio output stage failed to start");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 서피스(창) 생성 / 해체
// ---------------------------------------------------------------------------

bool PlayerCore::CreateSurface(int window_id, const WindowPlacement& placement,
                               const std::string& aspect_mode) {
  if (surfaces_.count(window_id)) {
    feedback_("warn", "create_window: window already exists: " + std::to_string(window_id));
    return true;
  }
  auto surf = std::make_unique<Surface>();
  Surface* s = surf.get();
  s->core = this;
  s->id = window_id;
  s->aspect_mode = aspect_mode;

  // 창 생성 (핸들러는 창 스레드 → 메인 마샬링). window_id를 캡처해 피드백에 반사.
  PlayerCore* self = this;
  const int wid = window_id;
  bool ok = s->window.Create(
      placement,
      [self, wid] {
        InvokeOnMain([self, wid] {
          if (self->on_surface_closed_) self->on_surface_closed_(wid);
        });
      },
      [self, wid](bool value) {
        InvokeOnMain([self, wid, value] {
          self->feedback_("set_fullscreen", json{{"window_id", wid}, {"value", value}});
        });
      },
      [self, wid](const WindowPlacement& p) {
        InvokeOnMain([self, wid, p] {
          self->SetDisplaySize(p.width, p.height, wid);
          self->feedback_("set_display", json{{"window_id", wid},
                                              {"monitor_index", p.monitor_index},
                                              {"x", p.x},
                                              {"y", p.y},
                                              {"width", p.width},
                                              {"height", p.height}});
        });
      });
  if (!ok) {
    feedback_("error", "create_window: failed to create window " + std::to_string(window_id));
    return false;
  }
  s->hwnd = s->window.hwnd();

  if (!BuildSurfaceGraph(s)) {
    s->window.Destroy();
    feedback_("error", "create_window: failed to build graph " + std::to_string(window_id));
    return false;
  }

  surfaces_[window_id] = std::move(surf);
  SetAspectMode(aspect_mode, s->window.client_width(), s->window.client_height(), window_id);
  feedback_("debug", "window created: " + std::to_string(window_id));
  return true;
}

bool PlayerCore::BuildSurfaceGraph(Surface* s) {
  const std::string sfx = std::to_string(s->id);
  s->comp = MakeElementN(use_d3d11_ ? "d3d11compositor" : "compositor", "comp" + sfx);
  s->vsink = MakeElementN(use_d3d11_ ? "d3d11videosink" : "autovideosink", "vsink" + sfx);
  if (!s->comp || !s->vsink) {
    feedback_("error", "surface: video output elements unavailable");
    return false;
  }
  // 배경을 검정으로 (기본값은 체커보드 무늬 — 배경이 순간 비면 그게 보임). d3d11compositor/
  // compositor 모두 background enum 1 = black.
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(s->comp), "background"))
    g_object_set(s->comp, "background", 1, nullptr);
  // 핵심: 실행 중(러닝타임 T>0)인 파이프라인에 컴포지터를 나중에 추가하므로, 애그리게이터가
  // 러닝타임 0부터 출력하면(기본 start-time-selection=zero) 클록(T)보다 과거라 전 프레임이
  // 버려지거나 정지한다. start-time-selection=1(first)로 첫 버퍼의 러닝타임(≈T)에서 시작해 정렬.
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(s->comp), "start-time-selection"))
    g_object_set(s->comp, "start-time-selection", 1, nullptr);
  // async=FALSE: 실행 중 파이프라인에 창을 나중에 추가할 때 프리롤 핸드셰이크 대기로 멈추는 것 방지(핵심).
  // QoS는 켜둔다: 디코드가 밀리면 늦은 프레임을 드롭해 실시간을 유지(버벅임 완화). max-lateness는
  // 기본값 사용 — 과거 정지 버그는 타이밍 정렬(async + start-time-selection)로 해결됐다.
  g_object_set(s->vsink, "qos", TRUE, "sync", TRUE, "async", FALSE, nullptr);

  s->bg_src = MakeElementN("videotestsrc", "bg" + sfx);
  g_object_set(s->bg_src, "pattern", 17 /* solid-color */, "is-live", TRUE, "foreground-color",
               (guint)0xFF000000, nullptr);
  GstElement* bg_caps = MakeElementN("capsfilter", "bg_caps" + sfx);
  {
    GstCaps* caps = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, kCanvasWidth, "height",
                                        G_TYPE_INT, kCanvasHeight, "framerate", GST_TYPE_FRACTION,
                                        kCanvasFps, 1, nullptr);
    g_object_set(bg_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }
  GstElement* bg_upload = use_d3d11_ ? MakeElementN("d3d11upload", "bg_upload" + sfx) : nullptr;

  gst_bin_add_many(GST_BIN(pipeline_), s->comp, s->vsink, s->bg_src, bg_caps, nullptr);
  if (bg_upload) gst_bin_add(GST_BIN(pipeline_), bg_upload);

  if (!gst_element_link(s->comp, s->vsink)) {
    feedback_("error", "surface: failed to link comp→vsink");
    return false;
  }
  {
    bool ok = bg_upload ? gst_element_link_many(s->bg_src, bg_caps, bg_upload, nullptr)
                        : gst_element_link(s->bg_src, bg_caps);
    GstElement* bg_end = bg_upload ? bg_upload : bg_caps;
    GstPad* src = gst_element_get_static_pad(bg_end, "src");
    GstPad* sink = gst_element_request_pad_simple(s->comp, "sink_%u");
    ok = ok && gst_pad_link(src, sink) == GST_PAD_LINK_OK;
    g_object_set(sink, "zorder", (guint)0, nullptr);
    gst_object_unref(src);
    gst_object_unref(sink);
    if (!ok) {
      feedback_("error", "surface: failed to link background branch");
      return false;
    }
  }

  if (!InitLogoBranch(s)) return false;

  // 파이프라인이 이미 PLAYING이므로 새 요소를 부모 상태에 동기화
  gst_element_sync_state_with_parent(s->comp);
  gst_element_sync_state_with_parent(s->vsink);
  gst_element_sync_state_with_parent(s->bg_src);
  gst_element_sync_state_with_parent(bg_caps);
  if (bg_upload) gst_element_sync_state_with_parent(bg_upload);
  // 실행 중 파이프라인에 라이브 브랜치(bg/컴포지터/싱크)를 추가했으니 지연 재계산 —
  // 안 하면 새 싱크가 stale 지연으로 프레임을 늦다고 버린다.
  gst_bin_recalculate_latency(GST_BIN(pipeline_));
  return true;
}

void PlayerCore::TeardownSurfaceGraph(Surface* s) {
  ClearPool(s);
  if (s->decks[0]) TeardownDeck(s->decks[0].get());
  if (s->decks[1]) TeardownDeck(s->decks[1].get());
  // comp/vsink/bg/logo를 파이프라인에서 제거 (bin_remove가 unref). 개별 요소 참조는 보관하지
  // 않으므로 이름으로 찾지 않고 add 시 저장한 포인터로 제거한다. 링크는 상태 NULL 후 자동 해제.
  auto drop = [&](GstElement* e) {
    if (!e) return;
    gst_element_set_locked_state(e, TRUE);
    gst_element_set_state(e, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(pipeline_), e);
  };
  // bg/logo의 보조 요소(caps/upload/queue)는 comp 제거 시 링크만 끊기고 파이프라인에 남으므로
  // 이름으로 함께 제거. (bg_caps/bg_upload/logo_queue/logo_upload)
  const std::string sfx = std::to_string(s->id);
  auto drop_named = [&](const std::string& name) {
    if (GstElement* e = gst_bin_get_by_name(GST_BIN(pipeline_), name.c_str())) {
      gst_element_set_locked_state(e, TRUE);
      gst_element_set_state(e, GST_STATE_NULL);
      gst_bin_remove(GST_BIN(pipeline_), e);  // get_by_name의 ref는 remove가 소비하지 않음
      gst_object_unref(e);
    }
  };
  drop(s->bg_src);
  drop_named("bg_caps" + sfx);
  drop_named("bg_upload" + sfx);
  drop(s->logo_src);
  drop_named("logo_queue" + sfx);
  drop_named("logo_upload" + sfx);
  drop(s->comp);
  drop(s->vsink);
  s->comp = s->vsink = s->bg_src = s->logo_src = nullptr;
  s->logo_pad = nullptr;
}

void PlayerCore::DestroySurface(int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  TeardownSurfaceGraph(s);
  s->window.Destroy();
  surfaces_.erase(window_id);
  feedback_("debug", "window destroyed: " + std::to_string(window_id));
}

json PlayerCore::ListSurfaces() const {
  json arr = json::array();
  for (const auto& [id, s] : surfaces_) {
    arr.push_back({{"window_id", id},
                   {"aspect_mode", s->aspect_mode},
                   {"width", s->window.client_width()},
                   {"height", s->window.client_height()},
                   {"fullscreen", s->window.IsFullscreen()}});
  }
  return arr;
}

json PlayerCore::EngineStats() const {
  int live = 0, prerolled = 0, pool = 0;
  for (const auto& [id, s] : surfaces_) {
    for (const auto& d : s->decks) {
      if (!d) continue;
      if (d->state == Deck::State::Live) ++live;
      else if (d->state == Deck::State::Prerolled) ++prerolled;
    }
    pool += static_cast<int>(s->pool.size());
    for (const auto& d : s->pool) {
      if (d && d->state == Deck::State::Prerolled) ++prerolled;
    }
  }
  return json{{"surfaces", static_cast<int>(surfaces_.size())},
              {"live_decks", live},
              {"prerolled_decks", prerolled},
              {"pool_decks", pool},
              {"audio_tracks", static_cast<int>(audio_tracks_.size())}};
}

void PlayerCore::SetMasterVolume(double volume) {
  master_volume_ = std::clamp(volume, 0.0, 100.0) / 100.0;
  if (master_vol_) g_object_set(master_vol_, "volume", master_volume_, nullptr);
}

void PlayerCore::ApplyDisplayPlacement(const WindowPlacement& placement, int window_id) {
  if (Surface* s = GetSurface(window_id)) s->window.ApplyPlacement(placement);
}

void PlayerCore::SetFullscreen(bool fullscreen, int window_id) {
  if (Surface* s = GetSurface(window_id)) s->window.SetFullscreen(fullscreen);
}

// ---------------------------------------------------------------------------
// 로고 오버레이 (창별)
// ---------------------------------------------------------------------------

bool PlayerCore::InitLogoBranch(Surface* s) {
  const std::string sfx = std::to_string(s->id);
  s->logo_src = MakeElementN("appsrc", "logo_src" + sfx);
  g_object_set(s->logo_src, "is-live", TRUE, "do-timestamp", FALSE, "format", GST_FORMAT_TIME,
               "min-latency", (gint64)0, "max-latency", (gint64)-1, nullptr);
  GstElement* logo_queue = MakeElementN("queue", "logo_queue" + sfx);
  GstElement* upload = use_d3d11_ ? MakeElementN("d3d11upload", "logo_upload" + sfx) : nullptr;

  gst_bin_add_many(GST_BIN(pipeline_), s->logo_src, logo_queue, nullptr);
  if (upload) gst_bin_add(GST_BIN(pipeline_), upload);

  GstElement* tail = upload ? upload : logo_queue;
  bool link_ok = gst_element_link(s->logo_src, logo_queue);
  if (upload) link_ok = link_ok && gst_element_link(logo_queue, upload);
  if (!link_ok) {
    feedback_("error", "logo: link failed");
    return false;
  }
  GstPad* src = gst_element_get_static_pad(tail, "src");
  s->logo_pad = gst_element_request_pad_simple(s->comp, "sink_%u");
  g_object_set(s->logo_pad, "zorder", (guint)100, "alpha", 0.0, nullptr);
  const bool ok = gst_pad_link(src, s->logo_pad) == GST_PAD_LINK_OK;
  gst_object_unref(src);
  if (!ok) {
    feedback_("error", "logo: link to compositor failed");
    return false;
  }
  gst_element_sync_state_with_parent(s->logo_src);
  gst_element_sync_state_with_parent(logo_queue);
  if (upload) gst_element_sync_state_with_parent(upload);

  const uint8_t transparent[4] = {0, 0, 0, 0};
  s->logo_img_w = 1;
  s->logo_img_h = 1;
  PushLogoBuffer(s, 1, 1, transparent);
  return true;
}

void PlayerCore::PushLogoBuffer(Surface* s, int w, int h, const uint8_t* rgba) {
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                      G_TYPE_INT, w, "height", G_TYPE_INT, h, "framerate",
                                      GST_TYPE_FRACTION, 0, 1, nullptr);
  gst_app_src_set_caps(GST_APP_SRC(s->logo_src), caps);
  gst_caps_unref(caps);

  const size_t size = static_cast<size_t>(w) * h * 4;
  GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
  gst_buffer_fill(buf, 0, rgba, size);

  GstState state = GST_STATE_NULL;
  gst_element_get_state(pipeline_, &state, nullptr, 0);
  GST_BUFFER_PTS(buf) = (state == GST_STATE_PLAYING) ? RunningTime() : 0;
  GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;

  gst_app_src_push_buffer(GST_APP_SRC(s->logo_src), buf);
}

void PlayerCore::ApplyLogoGeometry(Surface* s) {
  if (!s->logo_pad || s->logo_img_w <= 0 || s->logo_img_h <= 0) return;
  int disp_w = s->logo_size_px > 0 ? s->logo_size_px : s->logo_img_w;
  int disp_h = disp_w * s->logo_img_h / s->logo_img_w;
  if (disp_w > kCanvasWidth) {
    disp_w = kCanvasWidth;
    disp_h = disp_w * s->logo_img_h / s->logo_img_w;
  }
  if (disp_h > kCanvasHeight) {
    disp_h = kCanvasHeight;
    disp_w = disp_h * s->logo_img_w / s->logo_img_h;
  }
  g_object_set(s->logo_pad, "xpos", (kCanvasWidth - disp_w) / 2, "ypos",
               (kCanvasHeight - disp_h) / 2, "width", disp_w, "height", disp_h, nullptr);
}

void PlayerCore::UpdateLogoVisibility(Surface* s, bool emit_feedback) {
  const bool visible = s->logo_enabled && s->media_wants_logo && s->logo_loaded;
  if (s->logo_pad) g_object_set(s->logo_pad, "alpha", visible ? 1.0 : 0.0, nullptr);
  if (emit_feedback) feedback_("logo_visibility", json{{"window_id", s->id}, {"show", visible}});
}

void PlayerCore::SetLogoFile(const std::string& path, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  std::string ext = std::filesystem::path(path).extension().string();
  for (auto& c : ext) c = static_cast<char>(tolower(c));

  if (ext == ".svg") {
    auto document = lunasvg::Document::loadFromFile(path);
    if (!document) {
      feedback_("error", "logo: failed to load svg: " + path);
      return;
    }
    auto bitmap = document->renderToBitmap();
    if (!bitmap.valid()) {
      feedback_("error", "logo: failed to render svg: " + path);
      return;
    }
    bitmap.convertToRGBA();
    s->logo_img_w = static_cast<int>(bitmap.width());
    s->logo_img_h = static_cast<int>(bitmap.height());
    PushLogoBuffer(s, s->logo_img_w, s->logo_img_h, bitmap.data());
  } else {
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!pixels) {
      feedback_("error", "logo: failed to load image: " + path);
      return;
    }
    s->logo_img_w = w;
    s->logo_img_h = h;
    PushLogoBuffer(s, w, h, pixels);
    stbi_image_free(pixels);
  }

  s->logo_loaded = true;
  ApplyLogoGeometry(s);
  UpdateLogoVisibility(s, /*emit_feedback=*/false);
  feedback_("debug", "logo loaded: " + path);
}

void PlayerCore::SetLogoSize(int width_px, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  s->logo_size_px = width_px;
  ApplyLogoGeometry(s);
}

void PlayerCore::SetLogoEnabled(bool show, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  s->logo_enabled = show;
  UpdateLogoVisibility(s, /*emit_feedback=*/true);
}

void PlayerCore::Shutdown() {
  if (!pipeline_) return;
  // 대기 중인 동기 그룹의 타임아웃 소스 제거 (해체 후 콜백이 죽은 덱을 참조하지 않도록).
  if (sync_group_) {
    if (sync_group_->timeout_id) g_source_remove(sync_group_->timeout_id);
    sync_group_.reset();
  }
  for (auto& [id, t] : audio_tracks_) {
    if (t && t->preroll_watch) g_source_remove(t->preroll_watch);
  }
  audio_tracks_.clear();
  for (auto& [id, s] : surfaces_) {
    for (auto& d : s->decks) {
      if (d && d->preroll_watch) g_source_remove(d->preroll_watch);
    }
  }
  // 창/그래프 해체 (창 스레드 join 포함)
  std::vector<int> ids;
  for (auto& [id, s] : surfaces_) ids.push_back(id);
  for (int id : ids) DestroySurface(id);

  gst_element_set_state(pipeline_, GST_STATE_NULL);
  if (silence_pad_) {
    gst_object_unref(silence_pad_);
    silence_pad_ = nullptr;
  }
  if (ptp_clock_) {
    gst_object_unref(ptp_clock_);
    ptp_clock_ = nullptr;
  }
  gst_object_unref(pipeline_);
  pipeline_ = nullptr;
}

// ---------------------------------------------------------------------------
// 버스
// ---------------------------------------------------------------------------

PlayerCore::Surface* PlayerCore::SurfaceForVsink(GstElement* vsink) {
  for (auto& [id, s] : surfaces_) {
    if (s->vsink == vsink) return s.get();
  }
  return nullptr;
}

GstBusSyncReply PlayerCore::OnBusSync(GstBus*, GstMessage* msg, gpointer user_data) {
  auto* self = static_cast<PlayerCore*>(user_data);
  if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
    GstElement* src = GST_ELEMENT(GST_MESSAGE_SRC(msg));
    // 어느 창의 vsink가 보낸 메시지인지 매칭 (src가 vsink 자신 또는 그 하위일 수 있음)
    Surface* target = nullptr;
    for (auto& [id, s] : self->surfaces_) {
      if (s->vsink && (src == s->vsink || gst_object_has_as_ancestor(GST_OBJECT(src),
                                                                     GST_OBJECT(s->vsink)))) {
        target = s.get();
        break;
      }
    }
    if (target) {
      gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)),
                                          reinterpret_cast<guintptr>(target->hwnd));
      target->overlay_ready = true;
      self->ApplyVideoGeometry(target);
    }
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
      const std::string emsg = err ? err->message : "unknown";
      self->feedback_("error", "pipeline error from " + src + ": " + emsg);
      // 오디오 sink 열기 실패 → 무음 fakesink로 교체해 영상은 계속 재생 (파이프라인 정지 방지)
      const bool from_asink = src == "asink" || src.rfind("asink", 0) == 0;
      const bool resource_open =
          err && (err->domain == GST_RESOURCE_ERROR &&
                  (err->code == GST_RESOURCE_ERROR_OPEN_WRITE ||
                   err->code == GST_RESOURCE_ERROR_OPEN_READ_WRITE ||
                   err->code == GST_RESOURCE_ERROR_BUSY ||
                   err->code == GST_RESOURCE_ERROR_NOT_FOUND));
      if (from_asink || resource_open) self->FallbackAudioSink();
      if (err) g_error_free(err);
      g_free(dbg);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      std::string wsrc = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
      self->feedback_("warn", "[" + wsrc + "] " + std::string(err ? err->message : "warning") +
                                  (dbg ? std::string(" | ") + dbg : ""));
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
    GstElement* freeze = deck->is_image ? MakeElement("imagefreeze", nullptr) : nullptr;
    GstElement* freeze_caps = nullptr;
    if (freeze) {
      freeze_caps = MakeElement("capsfilter", nullptr);
      GstCaps* fcaps =
          gst_caps_new_simple("video/x-raw", "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
      g_object_set(freeze_caps, "caps", fcaps, nullptr);
      gst_caps_unref(fcaps);
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

    deck->video_block = gst_pad_add_probe(
        deck->video_out,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
        [](GstPad*, GstPadProbeInfo*, gpointer user_data) -> GstPadProbeReturn {
          static_cast<Deck*>(user_data)->video_ready = true;
          return GST_PAD_PROBE_OK;
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

    gst_pad_add_probe(
        deck->video_out, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        [](GstPad*, GstPadProbeInfo* info, gpointer user_data) -> GstPadProbeReturn {
          auto* d = static_cast<Deck*>(user_data);
          if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS &&
              d->state == Deck::State::Live && !d->eos_sent && !d->core->timeline_active_) {
            d->eos_sent = true;
            PlayerCore* c = d->core;
            const int track = d->track_idx;
            const int id = d->id;
            const int wid = d->surface ? d->surface->id : 0;
            InvokeOnMain([c, track, id, wid] {
              c->feedback_("end_reached", json{{"playlist_track_index", track},
                                               {"active_player_id", id},
                                               {"window_id", wid}});
            });
          }
          return GST_PAD_PROBE_OK;
        },
        deck, nullptr);
  } else if (is_audio && !deck->audio_tail) {
    GstElement* q = MakeElement("queue", nullptr);
    GstElement* conv = MakeElement("audioconvert", nullptr);
    GstElement* res = MakeElement("audioresample", nullptr);
    GstElement* capsf = MakeElement("capsfilter", nullptr);
    {
      GstCaps* bcaps = MakeBranchCaps(deck->branch_channels);
      g_object_set(capsf, "caps", bcaps, nullptr);
      gst_caps_unref(bcaps);
    }
    GstElement* vol = MakeElement("volume", nullptr);
    g_object_set(vol, "volume", deck->master_muted ? 0.0 : deck->volume_gain, nullptr);

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
}

std::unique_ptr<PlayerCore::Deck> PlayerCore::ConstructDeck(Surface* s, int id, const json& file,
                                                           int track_idx, bool play_when_ready,
                                                           double image_time_s, int64_t delay_ms) {
  const std::string path = file.value("path", "");
  auto uri = ToUri(path);
  if (!uri) {
    feedback_("error", "invalid media path: " + path);
    return nullptr;
  }

  auto deck = std::make_unique<Deck>();
  deck->core = this;
  deck->surface = s;
  deck->id = id;
  deck->file = file;
  deck->track_idx = track_idx;
  deck->play_when_ready = play_when_ready;
  deck->is_image = file.value("is_image", false) ||
                   file.value("mimetype", std::string()).rfind("image/", 0) == 0;
  deck->image_time_ms = static_cast<gint64>(image_time_s * 1000.0);
  deck->in_ms = deck->is_image ? 0 : file.value("in_ms", static_cast<gint64>(0));
  deck->delay_ms = std::max<gint64>(0, delay_ms);
  deck->start_at_rt = file.value("start_at", static_cast<int64_t>(-1));  // 멀티 PC 공유 시작 시각

  {
    StreamAudio sa = ParseFileAudio(file);
    deck->channel_routes = sa.routes;
    deck->volume_gain = sa.volume_gain;
    deck->master_muted = sa.master_muted;
    deck->branch_channels = sa.routes.empty() ? 2 : static_cast<int>(sa.routes.size());
  }

  static int name_seq = 0;  // 메인 스레드 전용 — bin 이름 고유화 (풀 덱 다수 대비)
  const std::string bin_name =
      "deck" + std::to_string(s->id) + "_" + std::to_string(id) + "_" + std::to_string(name_seq++);
  deck->bin = gst_bin_new(bin_name.c_str());
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

  return deck;
}

PlayerCore::Deck* PlayerCore::BuildDeck(Surface* s, int deck_id, const json& file, int track_idx,
                                        bool play_when_ready, double image_time_s,
                                        int64_t delay_ms) {
  if (s->decks[deck_id]) TeardownDeck(s->decks[deck_id].get());
  auto deck = ConstructDeck(s, deck_id, file, track_idx, play_when_ready, image_time_s, delay_ms);
  if (!deck) return nullptr;
  Deck* raw = deck.get();
  s->decks[deck_id] = std::move(deck);
  return raw;
}

// 프리롤 완료 후 실제 표시: 트랙별 delay_ms만큼 대기했다 스왑(그동안 배경색 유지).
// start_at 지정 시엔 delay_ms를 무시하고 즉시 링크한다 — 표시 시각은 pad offset(start_at)이
// 결정하므로, delay 타이머로 링크를 늦추면 오프셋이 과거로 밀릴 수 있다(동기 우선).
void PlayerCore::SwapWithDelay(Deck* deck) {
  if (deck->start_at_rt < 0 && deck->delay_ms > 0) {
    deck->delay_timer = g_timeout_add(
        static_cast<guint>(deck->delay_ms),
        [](gpointer data) -> gboolean {
          auto* dd = static_cast<Deck*>(data);
          dd->delay_timer = 0;
          if (dd->state == Deck::State::Prerolled) dd->core->SwapTo(dd);
          return G_SOURCE_REMOVE;
        },
        deck);
  } else {
    SwapTo(deck);
  }
}

bool PlayerCore::CheckPreroll(Deck* deck) {
  Surface* s = deck->surface;
  GstState state = GST_STATE_NULL;
  if (gst_element_get_state(deck->bin, &state, nullptr, 0) == GST_STATE_CHANGE_FAILURE) {
    feedback_("error", "deck preroll failed: " + deck->file.value("path", ""));
    TeardownDeck(deck);
    return false;
  }

  const GstClockTime now = gst_clock_get_time(gst_system_clock_obtain());
  const bool has_branch = deck->video_tail || deck->audio_tail;
  const bool video_ok = !deck->video_tail || deck->video_ready;
  const bool audio_ok = !deck->audio_tail || deck->audio_ready;
  if (has_branch && video_ok && audio_ok && now - deck->preroll_started > 300 * GST_MSECOND) {
    if (deck->in_ms > 0 && !deck->inpoint_seeked) {
      deck->inpoint_seeked = true;
      deck->video_ready = false;
      deck->audio_ready = false;
      GstPad* seek_pad = deck->video_out ? deck->video_out : deck->audio_out;
      if (seek_pad) {
        GstEvent* seek = gst_event_new_seek(
            1.0, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            GST_SEEK_TYPE_SET, deck->in_ms * GST_MSECOND, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
        gst_pad_send_event(seek_pad, seek);
      }
      deck->preroll_started = now;
      return true;
    }
    deck->state = Deck::State::Prerolled;
    if (deck->sync_pending) {
      // 동기 그룹 멤버: 개별 스왑 금지 — 배리어가 전원 준비 시 일괄 스왑을 소유.
      MaybeFireSyncGroup(/*force=*/false);
    } else if (deck->play_when_ready) {
      SwapWithDelay(deck);  // 트랙별 delay_ms 반영
    } else if (deck->id < 0) {
      // 풀 파킹 덱: 승격 전까지 대기 (standby 슬롯 미점유). UI 로딩 표시용 구조화 피드백.
      EmitPreloadStatus(s, "deck_prerolled", deck->track_idx, deck->file.value("path", std::string()));
    } else {
      s->standby_deck = deck->id;
      feedback_("debug", "deck " + std::to_string(deck->id) + " preloaded (win " +
                             std::to_string(s->id) + ")");
    }
    return false;
  }

  if (now - deck->preroll_started > kPrerollTimeout) {
    feedback_("error", "deck preroll timeout: " + deck->file.value("path", ""));
    TeardownDeck(deck);
    return false;
  }
  return true;
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

int64_t PlayerCore::GetRunningTimeNs() const { return static_cast<int64_t>(RunningTime()); }

// ---------------------------------------------------------------------------
// 멀티 PC PTP 클럭 동기 (Phase 5)
//
// PTP(IEEE 1588) = 멀티캐스트 기반 시각 동기. gst_ptp_init로 데몬 스레드 기동(UDP 319/320),
// gst_ptp_clock_new로 도메인 클럭 획득 → 파이프라인 클록으로 사용하면 전 PC가 같은 절대
// 시각을 공유한다. 러닝타임(clock-base_time)까지 동일하려면 base_time을 master값으로 맞춰야
// 하므로 slave는 SetPtpBaseTime으로 동기화한다. 기본은 시스템 클록 — 명시 호출 시에만 전환.
// ---------------------------------------------------------------------------

void PlayerCore::EnablePtp(int domain) {
  if (!gst_ptp_is_supported()) {
    feedback_("error", "ptp: not supported on this platform/build");
    return;
  }
  static bool inited = false;
  if (!inited) {
    if (!gst_ptp_init(GST_PTP_CLOCK_ID_NONE, nullptr)) {
      feedback_("error", "ptp: gst_ptp_init failed");
      return;
    }
    inited = true;
  }
  if (ptp_clock_) {
    gst_object_unref(ptp_clock_);
    ptp_clock_ = nullptr;
  }
  ptp_clock_ = gst_ptp_clock_new("vp-ptp", static_cast<guint>(domain));
  if (!ptp_clock_) {
    feedback_("error", "ptp: gst_ptp_clock_new failed (domain " + std::to_string(domain) + ")");
    return;
  }
  ptp_domain_ = domain;
  // 동기 확보 대기 (best-effort 2s) — 미동기여도 진행하고 상태로 보고
  gst_clock_wait_for_sync(ptp_clock_, 2 * GST_SECOND);
  gst_pipeline_use_clock(GST_PIPELINE(pipeline_), ptp_clock_);
  // 클록 교체 후 러닝타임을 깨끗하게 재설정: base_time = 현재 PTP 시각 → running_time≈0.
  // (동기 전 PTP 시각은 무효라 synced일 때만. master는 이 base_time을 slave에 배포한다.)
  if (gst_clock_is_synced(ptp_clock_)) {
    gst_element_set_base_time(pipeline_, gst_clock_get_time(ptp_clock_));
  }
  ptp_enabled_ = true;
  feedback_("ptp_status", PtpStatus());
}

void PlayerCore::SetPtpBaseTime(int64_t base_time) {
  // slave: master의 base_time에 맞춤 → RunningTime()=clock-base_time 전 PC 동일.
  // 재생 시작 전에 호출되는 것을 전제(재생 중 변경은 러닝타임을 흔들 수 있음).
  gst_element_set_base_time(pipeline_, static_cast<GstClockTime>(base_time));
  feedback_("ptp_status", PtpStatus());
}

nlohmann::json PlayerCore::PtpStatus() const {
  const bool synced = ptp_clock_ ? (bool)gst_clock_is_synced(ptp_clock_) : false;
  return nlohmann::json{{"enabled", ptp_enabled_},
                        {"domain", ptp_domain_},
                        {"synced", synced},
                        {"base_time", static_cast<int64_t>(gst_element_get_base_time(pipeline_))},
                        {"running_time", static_cast<int64_t>(RunningTime())}};
}

void PlayerCore::SwapTo(Deck* deck) {
  Surface* s = deck->surface;
  // 멀티 PC 동기: start_at 지정 시 pad offset을 공유 러닝타임 절대값으로 → 전 PC 동시 표시.
  // 미지정(-1)이면 로컬 현재 러닝타임 (기존 동작).
  const GstClockTime offset =
      deck->start_at_rt >= 0 ? static_cast<GstClockTime>(deck->start_at_rt) : RunningTime();

  if (deck->video_out) {
    deck->video_ghost = gst_ghost_pad_new("video_src", deck->video_out);
    gst_pad_set_active(deck->video_ghost, TRUE);
    gst_element_add_pad(deck->bin, deck->video_ghost);

    deck->comp_pad = gst_element_request_pad_simple(s->comp, "sink_%u");
    g_object_set(deck->comp_pad, "zorder", (guint)(1 + deck->id), "alpha", 1.0, "xpos", 0, "ypos",
                 0, "width", kCanvasWidth, "height", kCanvasHeight, nullptr);
    g_object_set(deck->comp_pad, "sizing-policy", 1, nullptr);
    gst_pad_set_offset(deck->video_out, static_cast<gint64>(offset));
    if (gst_pad_link(deck->video_ghost, deck->comp_pad) != GST_PAD_LINK_OK) {
      feedback_("error", "swap: video link to compositor failed");
    }
  }
  if (deck->audio_out) {
    deck->audio_ghost = gst_ghost_pad_new("audio_src", deck->audio_out);
    gst_pad_set_active(deck->audio_ghost, TRUE);
    gst_element_add_pad(deck->bin, deck->audio_ghost);

    deck->amix_pad = gst_element_request_pad_simple(amix_, "sink_%u");
    ApplyDeckRouting(deck);
    gst_pad_set_offset(deck->audio_out, static_cast<gint64>(offset));
    if (gst_pad_link(deck->audio_ghost, deck->amix_pad) != GST_PAD_LINK_OK) {
      feedback_("error", "swap: audio link to mixer failed");
    }
  }

  gst_element_set_locked_state(deck->bin, FALSE);
  gst_element_sync_state_with_parent(deck->bin);

  if (deck->video_block) {
    gst_pad_remove_probe(deck->video_out, deck->video_block);
    deck->video_block = 0;
  }
  if (deck->audio_block) {
    gst_pad_remove_probe(deck->audio_out, deck->audio_block);
    deck->audio_block = 0;
  }
  deck->state = Deck::State::Live;
  // 이 덱이 새로 라이브가 되므로 EOS 래치를 재무장한다. eos_sent는 한 번 true가 되면 다른 곳에서
  // 리셋되지 않으므로, 재사용/승격된 덱(풀 경로 등)이 이전 재생의 EOS 플래그를 물고 오면 이번
  // 재생의 end_reached가 영구 억제돼 장면이 안 넘어간다(에러 없이 멈춤). 라이브 전환마다 초기화.
  deck->eos_sent = false;
  s->paused = false;

  // 이전 라이브 덱: start_at이 미래면 그 시각까지 유지(장면 전환 시 검정 갭 방지), 그 순간
  // 숨김+해체. 들어오는 덱을 위로 올려두면 start_at에 첫 프레임이 뜨는 즉시 old를 덮는다
  // (크로스오버 무결). start_at이 현재/과거면 기존 즉시 하드컷.
  struct SwapCtx {
    PlayerCore* self;
    Surface* surf;
    int old_id;
    int new_id;
  };
  if (s->live_deck >= 0 && s->live_deck != deck->id && s->decks[s->live_deck]) {
    Deck* old = s->decks[s->live_deck].get();
    const gint64 now_rt = static_cast<gint64>(RunningTime());
    const gint64 hide_at = static_cast<gint64>(offset);  // 이번 스왑의 pad offset(= start_at)
    const gint64 wait_ms = (hide_at > now_rt) ? (hide_at - now_rt) / GST_MSECOND : 0;
    auto* ctx = new SwapCtx{this, s, old->id, deck->id};
    if (wait_ms > 0) {
      if (deck->comp_pad) g_object_set(deck->comp_pad, "zorder", (guint)90, nullptr);
      g_timeout_add(
          static_cast<guint>(wait_ms + 20),  // +~1프레임 여유
          [](gpointer data) -> gboolean {
            auto* c = static_cast<SwapCtx*>(data);
            Surface* sf = c->surf;
            if (sf->decks[c->old_id] && sf->live_deck != c->old_id) {
              Deck* o = sf->decks[c->old_id].get();
              if (o->comp_pad) g_object_set(o->comp_pad, "alpha", 0.0, nullptr);
              if (o->amix_pad) g_object_set(o->amix_pad, "mute", TRUE, nullptr);
              c->self->TeardownDeck(o);
            }
            // 남은(현재 라이브) 덱 zorder를 기본으로 복귀 → 다음 전환에서 새 덱이 다시 위로.
            if (sf->decks[c->new_id] && sf->decks[c->new_id]->comp_pad)
              g_object_set(sf->decks[c->new_id]->comp_pad, "zorder", (guint)(1 + c->new_id),
                           nullptr);
            delete c;
            return G_SOURCE_REMOVE;
          },
          ctx);
    } else {
      if (old->comp_pad) g_object_set(old->comp_pad, "alpha", 0.0, nullptr);
      if (old->amix_pad) g_object_set(old->amix_pad, "mute", TRUE, nullptr);
      g_timeout_add(
          100,
          [](gpointer data) -> gboolean {
            auto* c = static_cast<SwapCtx*>(data);
            Surface* sf = c->surf;
            if (sf->decks[c->old_id] && sf->live_deck != c->old_id)
              c->self->TeardownDeck(sf->decks[c->old_id].get());
            delete c;
            return G_SOURCE_REMOVE;
          },
          ctx);
    }
  }

  s->live_deck = deck->id;
  if (s->standby_deck == deck->id) s->standby_deck = -1;

  deck->image_started = offset;
  deck->image_elapsed_ms = 0;
  if (deck->is_image && deck->image_time_ms > 0) {
    deck->image_timer = g_timeout_add(
        static_cast<guint>(deck->image_time_ms),
        [](gpointer data) -> gboolean {
          auto* d = static_cast<Deck*>(data);
          d->image_timer = 0;
          if (d->state == Deck::State::Live && !d->eos_sent && !d->core->timeline_active_) {
            d->eos_sent = true;
            const int wid = d->surface ? d->surface->id : 0;
            d->core->feedback_("end_reached", json{{"playlist_track_index", d->track_idx},
                                                   {"active_player_id", d->id},
                                                   {"window_id", wid}});
          }
          return G_SOURCE_REMOVE;
        },
        deck);
  }

  s->media_wants_logo = (deck->video_tail == nullptr);
  UpdateLogoVisibility(s, /*emit_feedback=*/true);

  // 프리롤 풀 리필: 이 트랙이 라이브가 됐으니 다음 lookahead개를 미리 프리롤
  if (!timeline_active_ && !s->sequence.empty() && deck->track_idx >= 0) {
    FillPool(s, deck->track_idx);
  }

  if (!timeline_active_) {
    feedback_("active_player_id", json{{"id", deck->id}, {"window_id", s->id}});
    json changed = {{"idx", deck->id}, {"window_id", s->id}};
    if (deck->file.contains("uuid")) changed["uuid"] = deck->file["uuid"];
    if (deck->file.contains("path")) changed["path"] = deck->file["path"];
    if (deck->track_idx >= 0) {
      changed["playlist_track_index"] = deck->track_idx;
      track_index_ = deck->track_idx;
      feedback_("track_index", json{{"index", deck->track_idx}, {"window_id", s->id}});
    }
    feedback_("media_changed", changed);
  }
}

void PlayerCore::TeardownDeck(Deck* deck) {
  Surface* s = deck->surface;
  const int id = deck->id;
  // 대기 중인 동기 그룹에서 제거 (배리어가 죽은 포인터를 참조하지 않도록).
  if (sync_group_) {
    auto& m = sync_group_->members;
    m.erase(std::remove(m.begin(), m.end(), deck), m.end());
  }
  if (deck->preroll_watch) {
    g_source_remove(deck->preroll_watch);
    deck->preroll_watch = 0;
  }
  if (deck->delay_timer) {
    g_source_remove(deck->delay_timer);
    deck->delay_timer = 0;
  }
  if (deck->image_timer) {
    g_source_remove(deck->image_timer);
    deck->image_timer = 0;
  }
  deck->state = Deck::State::Dead;

  // 중요(데드락 방지): NULL 전환 전에 comp/amix에서 먼저 분리한다. 라이브 amix/comp에 연결된
  // 채로 bin을 NULL로 내리면 스트리밍 스레드가 믹서로의 push에서 막혀 set_state(NULL)이
  // 영구 대기(특정 오디오 디바이스/2덱 동시 상황에서 재현) → 정지가 전 창에 안 먹힘.
  // 고스트 패드를 먼저 언링크·요청 패드 해제해 bin을 고립시키면 NULL이 블록되지 않는다.
  if (deck->comp_pad) {
    if (deck->video_ghost) gst_pad_unlink(deck->video_ghost, deck->comp_pad);
    if (s && s->comp) gst_element_release_request_pad(s->comp, deck->comp_pad);
    gst_object_unref(deck->comp_pad);
    deck->comp_pad = nullptr;
  }
  if (deck->amix_pad) {
    if (deck->audio_ghost) gst_pad_unlink(deck->audio_ghost, deck->amix_pad);
    gst_element_release_request_pad(amix_, deck->amix_pad);
    gst_object_unref(deck->amix_pad);
    deck->amix_pad = nullptr;
  }

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

  if (deck->video_out) gst_object_unref(deck->video_out);
  if (deck->audio_out) gst_object_unref(deck->audio_out);

  gst_bin_remove(GST_BIN(pipeline_), deck->bin);

  // id>=0 = A/B 슬롯 덱만 decks[]에서 해제. id<0 = 풀 파킹 덱(소유는 s->pool, 호출자가 처리).
  if (s && id >= 0 && id < 2) {
    if (s->live_deck == id) s->live_deck = -1;
    if (s->standby_deck == id) s->standby_deck = -1;
    s->decks[id].reset();
  }
}

// ---------------------------------------------------------------------------
// 프리롤 풀 (전 트랙 프리롤 — 요구사항 #4)
// ---------------------------------------------------------------------------

int PlayerCore::PoolFindByPath(Surface* s, const std::string& path) {
  if (path.empty()) return -1;
  for (size_t i = 0; i < s->pool.size(); ++i) {
    if (!s->pool[i] || s->pool[i]->file.value("path", std::string()) != path) continue;
    // 프리롤 타임아웃 등으로 죽은(Dead) 덱은 풀에 남을 수 있다(TeardownDeck은 id<0 풀 덱을
    // s->pool에서 제거하지 않음). 이런 덱을 승격하면 절대 Live가 못 돼 play_synced가 조용히
    // 무한 대기한다. 사용 가능한(Building/Prerolled) 덱만 매칭해 신규 빌드로 폴백하게 한다.
    if (s->pool[i]->state == Deck::State::Building ||
        s->pool[i]->state == Deck::State::Prerolled)
      return static_cast<int>(i);
  }
  return -1;
}

int PlayerCore::CountPrerollDecks() const {
  int n = 0;
  for (const auto& [id, s] : surfaces_) {
    n += static_cast<int>(s->pool.size());
    if (s->standby_deck >= 0 && s->decks[s->standby_deck]) ++n;
  }
  return n;
}

void PlayerCore::ClearPool(Surface* s) {
  for (auto& d : s->pool) {
    if (d) TeardownDeck(d.get());
  }
  s->pool.clear();
  EmitPreloadStatus(s, "cleared", -1, std::string());  // 호스트 배지 리셋 (expected/prerolled=0)
}

// 프리롤 상태 구조화 피드백 (UI "로딩됨/로딩중" 표시). prerolled = 풀에서 Prerolled 상태 덱 수,
// expected = 현재 풀 크기. 모두 GLib 메인루프 스레드에서 호출 → feedback_ 직접 호출 안전.
void PlayerCore::EmitPreloadStatus(Surface* s, const char* event, int seq_idx,
                                   const std::string& path) {
  if (!s) return;
  int prerolled = 0;
  for (const auto& d : s->pool)
    if (d && d->state == Deck::State::Prerolled) ++prerolled;
  feedback_("preload_status",
            json{{"window_id", s->id},
                 {"event", event},
                 {"seq_idx", seq_idx},
                 {"path", path},
                 {"prerolled", prerolled},
                 {"expected", static_cast<int>(s->pool.size())},
                 {"sequence_len", static_cast<int>(s->sequence.size())}});
}

// 시퀀스의 한 인덱스를 풀에 프리롤 (이미 라이브/스탠바이/풀에 있으면 skip, 상한 초과면 false).
bool PlayerCore::PoolPrerollIndex(Surface* s, int seq_idx) {
  if (seq_idx < 0 || seq_idx >= static_cast<int>(s->sequence.size())) return true;
  const json& f = s->sequence[seq_idx];
  const std::string path = f.value("path", "");
  if (path.empty()) return true;
  for (const auto& d : s->decks) {
    if (d && d->file.value("path", std::string()) == path) return true;  // 라이브/스탠바이 커버
  }
  if (PoolFindByPath(s, path) >= 0) return true;  // 이미 풀에 있음
  if (CountPrerollDecks() >= preload_max_decks_) {
    feedback_("debug", "preload cap reached — track demoted to metadata: idx " +
                           std::to_string(seq_idx) + " (win " + std::to_string(s->id) + ")");
    return false;
  }
  auto deck = ConstructDeck(s, /*id=*/-1, f, seq_idx, /*play_when_ready=*/false,
                            f.value("time", 0.0), f.value("delay_ms", static_cast<int64_t>(0)));
  if (deck) s->pool.push_back(std::move(deck));
  return true;
}

void PlayerCore::FillPool(Surface* s, int current_idx) {
  if (s->sequence.empty()) return;
  // 죽은(Dead) 풀 덱 정리 — 프리롤 타임아웃으로 TeardownDeck된 뒤에도 id<0라 s->pool에 남는다.
  // 방치하면 cap 계산을 왜곡하고 재프리롤을 막는다. (메인 루프에서 호출되므로 erase 안전.)
  s->pool.erase(std::remove_if(s->pool.begin(), s->pool.end(),
                               [](const std::unique_ptr<Deck>& d) {
                                 return !d || d->state == Deck::State::Dead;
                               }),
                s->pool.end());
  for (int i = current_idx + 1; i <= current_idx + preload_lookahead_; ++i) {
    if (i >= static_cast<int>(s->sequence.size())) break;
    if (!PoolPrerollIndex(s, i)) break;  // 상한 도달 → 이후 트랙은 강등
  }
}

// 풀 덱을 A/B 슬롯으로 승격. 반환 = 배정된 슬롯. swap_now=true면 즉시(delay 반영) 스왑.
void PlayerCore::PromotePooled(Surface* s, int pool_idx, bool swap_now) {
  if (pool_idx < 0 || pool_idx >= static_cast<int>(s->pool.size())) return;
  auto deck = std::move(s->pool[pool_idx]);
  s->pool.erase(s->pool.begin() + pool_idx);

  const int slot = (s->live_deck == 0) ? 1 : 0;
  if (s->decks[slot]) TeardownDeck(s->decks[slot].get());
  deck->id = slot;
  Deck* raw = deck.get();
  s->decks[slot] = std::move(deck);

  if (swap_now) {
    raw->play_when_ready = true;
    if (raw->state == Deck::State::Prerolled) SwapWithDelay(raw);
    // else: 아직 프리롤 중 → CheckPreroll이 완료 시 스왑
  } else {
    if (raw->state == Deck::State::Prerolled) s->standby_deck = slot;
    // else: 프리롤 완료 시 CheckPreroll이 standby로 등록
  }
}

// ---------------------------------------------------------------------------
// 동기 그룹 (play_synced 배리어)
// ---------------------------------------------------------------------------

// 풀 승격 또는 신규 빌드하되 스왑은 보류. 반환 = 슬롯에 배정된 덱(없으면 nullptr).
PlayerCore::Deck* PlayerCore::PromoteOrBuildHeld(Surface* s, const json& file, int track_idx,
                                                 double image_time_s, int64_t delay_ms) {
  const int slot = (s->live_deck == 0) ? 1 : 0;
  const int pidx = PoolFindByPath(s, file.value("path", ""));
  if (pidx >= 0) {
    Deck* pd = s->pool[pidx].get();
    pd->image_time_ms = static_cast<gint64>(image_time_s * 1000.0);
    pd->delay_ms = std::max<gint64>(0, delay_ms);
    pd->track_idx = track_idx;
    PromotePooled(s, pidx, /*swap_now=*/false);  // 슬롯에 파킹(스왑 보류)
    return s->decks[slot].get();
  }
  if (s->standby_deck >= 0 && s->decks[s->standby_deck]) TeardownDeck(s->decks[s->standby_deck].get());
  return BuildDeck(s, slot, file, track_idx, /*play_when_ready=*/false, image_time_s, delay_ms);
}

void PlayerCore::MaybeFireSyncGroup(bool force) {
  if (!sync_group_) return;
  if (!force) {
    for (Deck* d : sync_group_->members)
      if (d && d->state == Deck::State::Building) return;  // 아직 프리롤 중인 멤버 존재
  }
  FireSyncGroup(force);
}

void PlayerCore::FireSyncGroup(bool force) {
  if (!sync_group_) return;
  auto g = std::move(sync_group_);  // 먼저 detach (SwapTo/FillPool 재진입 방지)
  if (g->timeout_id) g_source_remove(g->timeout_id);
  // start_at을 딱 한 번 계산 → 전 멤버가 동일 오프셋 → 동시 표시(락스텝).
  const gint64 start_at = (g->explicit_start_at >= 0)
                              ? g->explicit_start_at
                              : static_cast<gint64>(RunningTime()) + g->lead_ns;
  for (Deck* d : g->members) {
    if (!d) continue;
    d->sync_pending = false;
    if (d->state == Deck::State::Prerolled) {
      d->start_at_rt = start_at;
      SwapTo(d);
    } else {
      // force(타임아웃) 경로에서 아직 Building → 동기 포기, 준비되면 자체 러닝타임으로 스왑
      d->start_at_rt = -1;
      d->play_when_ready = true;
    }
  }
  (void)force;
}

void PlayerCore::PlaySynced(const json& msg) {
  if (sync_group_) FireSyncGroup(/*force=*/true);  // 이전 대기 그룹 강제 발화(정리)
  // 이 시점 sync_group_ == null → 아래 PromoteOrBuildHeld 안에서 TeardownDeck이 불려도
  // 죽은 그룹 참조 없음. 멤버는 로컬 g에 축적한 뒤 마지막에 sync_group_로 대입한다.

  auto g = std::make_unique<SyncGroup>();
  g->scene_idx = msg.value("scene_idx", -1);
  g->lead_ns = std::max<int64_t>(0, msg.value("lead_ms", 120)) * GST_MSECOND;
  g->explicit_start_at = msg.value("start_at", static_cast<int64_t>(-1));
  const int timeout_ms = std::clamp(msg.value("timeout_ms", 1500), 100, 10000);

  if (msg.contains("clips") && msg["clips"].is_array()) {
    for (const auto& c : msg["clips"]) {
      const int wid = c.value("window_id", 0);
      Surface* s = GetSurface(wid);
      if (!s || !c.contains("current") || c["current"].is_null()) continue;
      const int track_idx = c.value("track_idx", g->scene_idx);
      const double t = c.value("current_time", c["current"].value("time", 0.0));
      const int64_t delay_ms = c["current"].value("delay_ms", static_cast<int64_t>(0));
      Deck* d = PromoteOrBuildHeld(s, c["current"], track_idx, t, delay_ms);
      if (d) {
        d->sync_pending = true;
        g->members.push_back(d);
      }
      if (c.contains("next") && !c["next"].is_null()) {
        const double nt = c.value("next_time", c["next"].value("time", 0.0));
        PreloadNext(c["next"], track_idx >= 0 ? track_idx + 1 : -1, nt, d ? d->id : -1, wid);
      }
    }
  }

  const int scene_idx = g->scene_idx;
  sync_group_ = std::move(g);
  MaybeFireSyncGroup(/*force=*/false);  // 이미 프리롤된 멤버만 있으면 즉시 발화
  if (sync_group_) {                     // 아직 대기 → 타임아웃 무장 (Building 멤버 커버)
    sync_group_->timeout_id = g_timeout_add(
        static_cast<guint>(timeout_ms),
        [](gpointer self) -> gboolean {
          auto* core = static_cast<PlayerCore*>(self);
          if (core->sync_group_) core->sync_group_->timeout_id = 0;  // 자기 자신 → 중복 remove 방지
          core->MaybeFireSyncGroup(/*force=*/true);
          return G_SOURCE_REMOVE;
        },
        this);
  }

  // 자가치유 워치독 무장 — 이 play_synced가 요구한 클립들이 kLivenessHealMs 안에 실제로 Live가
  // 됐는지 나중에 점검한다. 배리어/프리롤/스왑의 런타임 스톨(죽은 풀 덱, 스왑 누락 등)로 특정 창이
  // 안 뜨면 장면 컨트롤러(호스트)가 end_reached를 영원히 못 받아 조용히 멈추는데, 이를 플레이어가
  // 스스로 재빌드로 복구한다. 세대(gen)를 캡처해 그 사이 새 play_synced가 오면 무효 처리.
  const uint64_t gen = ++sync_generation_;
  auto items = std::make_unique<std::vector<HealItem>>();
  if (msg.contains("clips") && msg["clips"].is_array()) {
    for (const auto& c : msg["clips"]) {
      if (!c.contains("current") || c["current"].is_null()) continue;
      HealItem hi;
      hi.window_id = c.value("window_id", 0);
      hi.track_idx = c.value("track_idx", scene_idx);
      hi.image_time_s = c.value("current_time", c["current"].value("time", 0.0));
      hi.delay_ms = c["current"].value("delay_ms", static_cast<int64_t>(0));
      hi.current = c["current"];
      items->push_back(std::move(hi));
    }
  }
  if (!items->empty()) {
    struct HealCtx {
      PlayerCore* self;
      uint64_t gen;
      std::unique_ptr<std::vector<HealItem>> items;
    };
    auto* ctx = new HealCtx{this, gen, std::move(items)};
    g_timeout_add(
        kLivenessHealMs,
        [](gpointer data) -> gboolean {
          auto* c = static_cast<HealCtx*>(data);
          c->self->HealSceneLiveness(c->gen, *c->items);
          delete c;
          return G_SOURCE_REMOVE;
        },
        ctx);
  }
}

// play_synced 자가치유: 기대 클립이 Live가 되지 못한 창을 복구한다.
void PlayerCore::HealSceneLiveness(uint64_t gen, const std::vector<HealItem>& items) {
  if (gen != sync_generation_) return;  // 그 사이 새 play_synced가 왔음 → stale, 복구 금지
  for (const auto& it : items) {
    Surface* s = GetSurface(it.window_id);
    if (!s) continue;
    const std::string& path = it.current.value("path", std::string());
    // 이미 기대 클립이 Live면 정상 — 아무것도 안 함.
    if (s->live_deck >= 0 && s->decks[s->live_deck] &&
        s->decks[s->live_deck]->state == Deck::State::Live &&
        s->decks[s->live_deck]->file.value("path", std::string()) == path)
      continue;

    feedback_("warn", "scene liveness heal: win " + std::to_string(it.window_id) +
                          " scene " + std::to_string(it.track_idx) +
                          " clip not live — recovering (" + path + ")");

    // 1) 슬롯에 이미 Prerolled인 동일 클립이 있으면 즉시 스왑.
    bool healed = false;
    for (int slot = 0; slot < 2 && !healed; ++slot) {
      Deck* d = s->decks[slot].get();
      if (d && d->state == Deck::State::Prerolled &&
          d->file.value("path", std::string()) == path) {
        d->track_idx = it.track_idx;
        d->sync_pending = false;
        d->start_at_rt = -1;  // 단독 복구 → 즉시(로컬 러닝타임) 스왑
        SwapTo(d);
        healed = true;
      }
    }
    // 2) 풀에 Prerolled 동일 클립이 있으면 승격+즉시 스왑.
    if (!healed) {
      const int pidx = PoolFindByPath(s, path);
      if (pidx >= 0 && s->pool[pidx]->state == Deck::State::Prerolled) {
        s->pool[pidx]->track_idx = it.track_idx;
        s->pool[pidx]->start_at_rt = -1;
        PromotePooled(s, pidx, /*swap_now=*/true);
        healed = true;
      }
    }
    // 3) 최후: 대기 슬롯에 신규 빌드(프리롤 완료 시 CheckPreroll이 자동 스왑).
    if (!healed) {
      const int slot = (s->live_deck == 0) ? 1 : 0;
      BuildDeck(s, slot, it.current, it.track_idx, /*play_when_ready=*/true, it.image_time_s,
                it.delay_ms);
    }
  }
}

void PlayerCore::SetPreloadConfig(const json& msg) {
  if (msg.contains("lookahead"))
    preload_lookahead_ = std::clamp(msg.value("lookahead", 1), 0, 32);
  if (msg.contains("max_decks"))
    preload_max_decks_ = std::clamp(msg.value("max_decks", 8), 1, 64);
  feedback_("debug", "preload config: lookahead=" + std::to_string(preload_lookahead_) +
                         " max_decks=" + std::to_string(preload_max_decks_));
}

void PlayerCore::PreloadPlaylist(const json& msg) {
  const int window_id = msg.value("window_id", 0);
  Surface* s = GetSurface(window_id);
  if (!s) {
    feedback_("error", "preload_playlist: unknown window " + std::to_string(window_id));
    return;
  }
  ClearPool(s);
  s->sequence.clear();
  if (msg.contains("tracks") && msg["tracks"].is_array()) {
    for (const auto& t : msg["tracks"]) s->sequence.push_back(t);
  }
  const int start = std::clamp(msg.value("current_index", 0), 0,
                               std::max(0, static_cast<int>(s->sequence.size()) - 1));
  // current 포함 [start .. start+lookahead] 프리롤 → 첫 재생도 지연 없이 승격 가능
  for (int i = start; i <= start + preload_lookahead_; ++i) {
    if (i >= static_cast<int>(s->sequence.size())) break;
    if (!PoolPrerollIndex(s, i)) break;
  }
  feedback_("debug", "preload_playlist: win " + std::to_string(window_id) + ", " +
                         std::to_string(s->sequence.size()) + " tracks, pool " +
                         std::to_string(s->pool.size()));
  // 호스트가 expected(풀 크기)를 미리 알도록 통지 (대부분 아직 Building → prerolled=0에서 시작).
  EmitPreloadStatus(s, "playlist_set", start, std::string());
}

// ---------------------------------------------------------------------------
// 공개 명령
// ---------------------------------------------------------------------------

int PlayerCore::PlayFile(const json& file, int track_idx, double image_time_s, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) {
    feedback_("error", "play: unknown window " + std::to_string(window_id));
    return -1;
  }
  // 프리롤 풀에 같은 파일이 있으면 즉시 승격 (지연 없는 전환)
  const int pidx = PoolFindByPath(s, file.value("path", ""));
  if (pidx >= 0) {
    Deck* pd = s->pool[pidx].get();
    pd->image_time_ms = static_cast<gint64>(image_time_s * 1000.0);
    pd->delay_ms = std::max<gint64>(0, file.value("delay_ms", static_cast<int64_t>(0)));
    pd->track_idx = track_idx;
    // 풀 승격 시에도 이번 재생의 start_at을 반드시 전파 (기본 -1 = 로컬 RunningTime 폴백).
    // 이게 빠지면 프리로드된 트랙이 동기 시작을 잃는다.
    pd->start_at_rt = file.value("start_at", static_cast<int64_t>(-1));
    const int slot = (s->live_deck == 0) ? 1 : 0;
    PromotePooled(s, pidx, /*swap_now=*/true);
    return slot;
  }
  const int slot = (s->live_deck == 0) ? 1 : 0;
  if (s->standby_deck >= 0 && s->decks[s->standby_deck]) TeardownDeck(s->decks[s->standby_deck].get());
  const int64_t delay_ms = file.value("delay_ms", static_cast<int64_t>(0));
  BuildDeck(s, slot, file, track_idx, /*play_when_ready=*/true, image_time_s, delay_ms);
  return slot;
}

void PlayerCore::PreloadNext(const json& file, int track_idx, double image_time_s, int avoid_slot,
                             int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  // 이미 풀에 프리롤돼 있으면 재빌드 불필요 (Next/play가 승격)
  if (PoolFindByPath(s, file.value("path", "")) >= 0) {
    feedback_("debug", "preload_next: already pooled");
    return;
  }
  int slot = (s->live_deck == 0) ? 1 : 0;
  if (slot == avoid_slot) slot = 1 - slot;
  if (slot == s->live_deck) {
    feedback_("debug", "preload_next skipped: no free slot");
    return;
  }
  const int64_t delay_ms = file.value("delay_ms", static_cast<int64_t>(0));
  BuildDeck(s, slot, file, track_idx, /*play_when_ready=*/false, image_time_s, delay_ms);
}

bool PlayerCore::DeckPrerolled(Surface* s, int slot) const {
  return s && slot >= 0 && slot < 2 && s->decks[slot] &&
         s->decks[slot]->state == Deck::State::Prerolled;
}

bool PlayerCore::Next(int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return false;
  if (s->standby_deck >= 0 && s->decks[s->standby_deck] &&
      s->decks[s->standby_deck]->state == Deck::State::Prerolled) {
    SwapTo(s->decks[s->standby_deck].get());
    return true;
  }
  // 프리롤 풀: 시퀀스상 다음 트랙이 풀에 있으면 승격
  if (!s->sequence.empty() && s->live_deck >= 0 && s->decks[s->live_deck]) {
    const int cur = s->decks[s->live_deck]->track_idx;
    if (cur >= 0 && cur + 1 < static_cast<int>(s->sequence.size())) {
      const int pidx = PoolFindByPath(s, s->sequence[cur + 1].value("path", std::string()));
      if (pidx >= 0) {
        PromotePooled(s, pidx, /*swap_now=*/true);
        return true;
      }
    }
  }
  // 폴백: 프리로드된 덱이 없으면 내부 tracks_로 다음 트랙 직접 재생 (레거시, 주 창 한정)
  const int n = static_cast<int>(tracks_.size());
  if (n > 0 && window_id == 0) {
    const int cur = (s->live_deck >= 0 && s->decks[s->live_deck] &&
                     s->decks[s->live_deck]->track_idx >= 0)
                        ? s->decks[s->live_deck]->track_idx
                        : track_index_;
    return PlayTrackIndex((cur + 1) % n);
  }
  feedback_("warn", "next: no preloaded deck");
  return false;
}

void PlayerCore::Play(int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s || s->live_deck < 0 || !s->decks[s->live_deck]) return;
  Deck* deck = s->decks[s->live_deck].get();
  if (!s->paused) return;

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
              const int wid = d->surface ? d->surface->id : 0;
              d->core->feedback_("end_reached", json{{"playlist_track_index", d->track_idx},
                                                     {"active_player_id", d->id},
                                                     {"window_id", wid}});
            }
            return G_SOURCE_REMOVE;
          },
          deck);
    }
  }
  s->paused = false;
}

void PlayerCore::Pause(int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s || s->live_deck < 0 || !s->decks[s->live_deck] || s->paused) return;
  Deck* deck = s->decks[s->live_deck].get();
  deck->paused_running = RunningTime();
  if (deck->video_out && !deck->video_block) {
    deck->video_block = gst_pad_add_probe(
        deck->video_out,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER), nullptr,
        nullptr, nullptr);
  }
  if (deck->audio_out && !deck->audio_block) {
    deck->audio_block = gst_pad_add_probe(
        deck->audio_out,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER), nullptr,
        nullptr, nullptr);
  }

  if (deck->is_image) {
    deck->image_elapsed_ms +=
        static_cast<gint64>((RunningTime() - deck->image_started) / GST_MSECOND);
    if (deck->image_timer) {
      g_source_remove(deck->image_timer);
      deck->image_timer = 0;
    }
  }
  s->paused = true;
}

void PlayerCore::Stop(int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  if (s->live_deck >= 0 && s->decks[s->live_deck]) TeardownDeck(s->decks[s->live_deck].get());
  if (s->standby_deck >= 0 && s->decks[s->standby_deck]) TeardownDeck(s->decks[s->standby_deck].get());
  ClearPool(s);  // 프리롤 풀 해제 (메모리 반환)
  s->sequence.clear();
  s->paused = false;
  s->media_wants_logo = true;
  UpdateLogoVisibility(s, /*emit_feedback=*/true);
}

void PlayerCore::StopAll() {
  std::vector<int> ids;
  for (auto& [id, s] : surfaces_) ids.push_back(id);
  for (int id : ids) Stop(id);
  StopAllAudioTracks();
}

void PlayerCore::SeekMs(int64_t time_ms, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s || s->live_deck < 0 || !s->decks[s->live_deck]) return;
  Deck* deck = s->decks[s->live_deck].get();
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

  const GstClockTime offset = RunningTime();
  if (deck->video_out) gst_pad_set_offset(deck->video_out, static_cast<gint64>(offset));
  if (deck->audio_out) gst_pad_set_offset(deck->audio_out, static_cast<gint64>(offset));
}

int PlayerCore::ActiveDeckId(int window_id) const {
  const Surface* s = GetSurface(window_id);
  return s ? s->live_deck : -1;
}

void PlayerCore::SetBackgroundColor(uint32_t rgb, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  if (s->bg_src) g_object_set(s->bg_src, "foreground-color", (guint)(0xFF000000u | rgb), nullptr);
  s->window.SetBackgroundColor(rgb);
}

void PlayerCore::SetAspectMode(const std::string& mode, int target_w, int target_h,
                               int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  s->aspect_mode = mode;
  if (target_w > 0) s->aspect_target_w = target_w;
  if (target_h > 0) s->aspect_target_h = target_h;
  ApplyVideoGeometry(s);
}

void PlayerCore::SetDisplaySize(int target_w, int target_h, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s) return;
  if (target_w > 0) s->aspect_target_w = target_w;
  if (target_h > 0) s->aspect_target_h = target_h;
  ApplyVideoGeometry(s);
}

void PlayerCore::ApplyVideoGeometry(Surface* s) {
  if (!s->overlay_ready || !s->vsink || !GST_IS_VIDEO_OVERLAY(s->vsink)) return;

  const int W = s->aspect_target_w > 0 ? s->aspect_target_w : kCanvasWidth;
  const int H = s->aspect_target_h > 0 ? s->aspect_target_h : kCanvasHeight;

  if (s->aspect_mode == "crop") {
    g_object_set(s->vsink, "force-aspect-ratio", TRUE, nullptr);
    const double canvas_ar = static_cast<double>(kCanvasWidth) / kCanvasHeight;
    int rw, rh;
    if (static_cast<double>(W) / H > canvas_ar) {
      rw = W;
      rh = static_cast<int>(W / canvas_ar + 0.5);
    } else {
      rh = H;
      rw = static_cast<int>(H * canvas_ar + 0.5);
    }
    const int rx = (W - rw) / 2;
    const int ry = (H - rh) / 2;
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(s->vsink), rx, ry, rw, rh);
  } else {
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(s->vsink), 0, 0, W, H);
    g_object_set(s->vsink, "force-aspect-ratio", s->aspect_mode == "letterbox" ? TRUE : FALSE,
                 nullptr);
  }
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(s->vsink));
}

namespace {

int DeviceCapsChannels(GstDevice* dev) {
  GstCaps* caps = gst_device_get_caps(dev);
  if (!caps) return 0;
  int channels = 0;
  for (guint i = 0; i < gst_caps_get_size(caps); i++) {
    const GstStructure* st = gst_caps_get_structure(caps, i);
    const GValue* v = gst_structure_get_value(st, "channels");
    if (!v) continue;
    if (G_VALUE_HOLDS_INT(v)) {
      channels = std::max(channels, g_value_get_int(v));
    } else if (GST_VALUE_HOLDS_INT_RANGE(v)) {
      channels = std::max(channels, gst_value_get_int_range_max(v));
    }
  }
  gst_caps_unref(caps);
  return channels;
}

void AppendProviderDevices(nlohmann::json& devices, const char* factory_name, const char* id_prop,
                           const char* id_prefix, const char* type) {
  GstDeviceProviderFactory* factory = gst_device_provider_factory_find(factory_name);
  GstDeviceProvider* provider = factory ? gst_device_provider_factory_get(factory) : nullptr;
  if (factory) gst_object_unref(factory);
  if (!provider) return;

  gst_device_provider_start(provider);
  GList* list = gst_device_provider_get_devices(provider);
  for (GList* it = list; it; it = it->next) {
    GstDevice* dev = GST_DEVICE(it->data);
    if (!gst_device_has_classes(dev, "Audio/Sink")) continue;
    gchar* name = gst_device_get_display_name(dev);
    GstStructure* props = gst_device_get_properties(dev);
    const gchar* dev_id = props ? gst_structure_get_string(props, id_prop) : nullptr;
    nlohmann::json entry = {
        {"deviceId", std::string(id_prefix) + (dev_id ? dev_id : (name ? name : ""))},
        {"name", name ? name : ""},
        {"type", type}};
    if (const int ch = DeviceCapsChannels(dev); ch > 0) {
      entry["channels"] = (std::string(type) == "wasapi") ? std::min(ch, 8) : ch;
    }
    devices.push_back(std::move(entry));
    if (props) gst_structure_free(props);
    g_free(name);
  }
  g_list_free_full(list, gst_object_unref);
  gst_device_provider_stop(provider);
  gst_object_unref(provider);
}

}  // namespace

json PlayerCore::ListAudioDevices() {
  json devices = json::array();
  AppendProviderDevices(devices, "wasapi2deviceprovider", "device.id", "", "wasapi");
  AppendProviderDevices(devices, "asiodeviceprovider", "device.clsid", "asio:", "asio");
  return devices;
}

void PlayerCore::SetAudioDevice(const std::string& device_id) {
  if (!audio_tail_ || !audio_sink_) return;

  const bool is_asio = device_id.rfind("asio:", 0) == 0;
  int channels = 0;
  if (!device_id.empty()) {
    for (const auto& d : ListAudioDevices()) {
      if (d.value("deviceId", std::string()) == device_id) {
        channels = d.value("channels", 0);
        break;
      }
    }
  }
  if (is_asio && channels < 1) {
    feedback_("error", "set_audio_device: asio device not available: " + device_id);
    return;
  }
  const bool positioned = !is_asio;
  if (!is_asio) channels = std::clamp(channels, 2, 8);

  struct Ctx {
    PlayerCore* core;
    std::string device;
    int channels;
    bool positioned;
  };
  auto* ctx = new Ctx{this, device_id, channels, positioned};

  GstPad* src = gst_element_get_static_pad(audio_tail_, "src");
  gst_pad_add_probe(
      src, GST_PAD_PROBE_TYPE_IDLE,
      [](GstPad*, GstPadProbeInfo*, gpointer data) -> GstPadProbeReturn {
        auto* c = static_cast<Ctx*>(data);
        c->core->DoAudioSinkSwap(c->device, c->channels, c->positioned);
        return GST_PAD_PROBE_REMOVE;
      },
      ctx, [](gpointer data) { delete static_cast<Ctx*>(data); });
  gst_object_unref(src);
}

// 출력 채널별 지연 링버퍼 재구성 (락 보유 상태에서 호출) — output_channels_ 기준.
static void RebuildRingsLocked(int out_ch, const std::vector<int>& delay_ms,
                               std::vector<int>& delay_samples,
                               std::vector<std::vector<float>>& rings, std::vector<int>& wpos,
                               int& ring_len, bool& active) {
  const int N = std::max(0, out_ch);
  delay_samples.assign(N, 0);
  int maxd = 0;
  for (int c = 0; c < N; ++c) {
    const int ms = c < static_cast<int>(delay_ms.size()) ? std::max(0, delay_ms[c]) : 0;
    const int s = ms * 48;  // 48kHz
    delay_samples[c] = s;
    maxd = std::max(maxd, s);
  }
  ring_len = maxd + 1;
  rings.assign(N, std::vector<float>(ring_len, 0.0f));
  wpos.assign(N, 0);
  active = maxd > 0;
}

void PlayerCore::SetChannelDelays(const json& delays) {
  std::lock_guard<std::mutex> lk(delay_mtx_);
  channel_delay_ms_.clear();
  if (delays.is_array()) {
    for (const auto& v : delays)
      channel_delay_ms_.push_back(v.is_number() ? std::max(0, static_cast<int>(v.get<double>())) : 0);
  }
  RebuildRingsLocked(output_channels_, channel_delay_ms_, chan_delay_samples_, chan_ring_,
                     chan_wpos_, delay_ring_len_, delays_active_);
  int maxms = 0;
  for (int m : channel_delay_ms_) maxms = std::max(maxms, m);
  feedback_("debug", "channel delays set (" + std::to_string(channel_delay_ms_.size()) +
                         " ch, max " + std::to_string(maxms) + "ms)");
}

void PlayerCore::RebuildDelayRings() {
  std::lock_guard<std::mutex> lk(delay_mtx_);
  RebuildRingsLocked(output_channels_, channel_delay_ms_, chan_delay_samples_, chan_ring_,
                     chan_wpos_, delay_ring_len_, delays_active_);
}

GstPadProbeReturn PlayerCore::OnBusAudioProbe(GstPad*, GstPadProbeInfo* info, gpointer user) {
  auto* self = static_cast<PlayerCore*>(user);
  if (!self->delays_active_) return GST_PAD_PROBE_OK;  // 패스스루 (racy read 무해)

  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  buf = gst_buffer_make_writable(buf);
  GST_PAD_PROBE_INFO_DATA(info) = buf;

  GstMapInfo map;
  if (!gst_buffer_map(buf, &map, GST_MAP_READWRITE)) return GST_PAD_PROBE_OK;

  std::lock_guard<std::mutex> lk(self->delay_mtx_);
  const int N = self->output_channels_;
  if (N <= 0 || static_cast<int>(self->chan_ring_.size()) != N) {
    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
  }
  const int L = self->delay_ring_len_;
  float* d = reinterpret_cast<float*>(map.data);
  const int frames = static_cast<int>(map.size / (sizeof(float) * N));
  for (int f = 0; f < frames; ++f) {
    for (int c = 0; c < N; ++c) {
      auto& ring = self->chan_ring_[c];
      int wp = self->chan_wpos_[c];
      ring[wp] = d[f * N + c];              // 입력 저장
      int rp = wp - self->chan_delay_samples_[c];  // 지연만큼 과거 샘플 읽기
      if (rp < 0) rp += L;
      d[f * N + c] = ring[rp];
      self->chan_wpos_[c] = (wp + 1) % L;
    }
  }
  gst_buffer_unmap(buf, &map);
  return GST_PAD_PROBE_OK;
}

// 오디오 sink가 열리지 않을 때(디바이스 사용 불가/점유/포맷 미지원) 무음 fakesink로 교체.
// 그렇지 않으면 sink가 버퍼를 소비 못 해 amix→덱 오디오 경로가 back-pressure로 막히고,
// 결국 영상까지 멈춘다("재생되다 멈춤"). fakesink(sync=true)는 클록에 맞춰 소비만 하므로
// 파이프라인이 계속 흐른다(오디오는 무음). 디바이스 재선택 시 정상 sink로 복귀.
void PlayerCore::FallbackAudioSink() {
  if (audio_fallback_active_) return;
  audio_fallback_active_ = true;
  gst_element_set_locked_state(audio_sink_, TRUE);
  gst_element_set_state(audio_sink_, GST_STATE_NULL);
  gst_element_unlink(audio_tail_, audio_sink_);
  gst_bin_remove(GST_BIN(pipeline_), audio_sink_);

  audio_sink_ = MakeElement("fakesink", "asink");
  g_object_set(audio_sink_, "sync", TRUE, "async", FALSE, "silent", TRUE, nullptr);
  gst_bin_add(GST_BIN(pipeline_), audio_sink_);
  gst_element_link(audio_tail_, audio_sink_);
  gst_element_sync_state_with_parent(audio_sink_);
  feedback_("warn", "audio device could not be opened — running silent (video continues)");
}

void PlayerCore::DoAudioSinkSwap(const std::string& device_id, int channels, bool positioned) {
  audio_fallback_active_ = false;  // 사용자가 디바이스를 다시 고르면 폴백 해제
  gst_element_set_locked_state(audio_sink_, TRUE);
  gst_element_set_state(audio_sink_, GST_STATE_NULL);
  gst_element_unlink(audio_tail_, audio_sink_);
  gst_bin_remove(GST_BIN(pipeline_), audio_sink_);

  if (channels != output_channels_ || positioned != bus_positioned_) {
    output_channels_ = channels;
    bus_positioned_ = positioned;
    GstCaps* caps = MakeBusCaps(channels, positioned);
    g_object_set(bus_caps_, "caps", caps, nullptr);
    gst_caps_unref(caps);
    if (silence_pad_) SetPadMatrix(silence_pad_, 1, channels, {-1});
    for (auto& [id, s] : surfaces_) {
      for (auto& d : s->decks) {
        if (d && d->amix_pad) ApplyDeckRouting(d.get());
      }
    }
    for (auto& [id, t] : audio_tracks_) {
      if (t && t->amix_pad)
        SetPadMatrix(t->amix_pad, t->branch_channels, output_channels_, t->channel_routes);
    }
    RebuildDelayRings();  // 출력 채널수 변경 → 지연 링버퍼 재구성 (delays 유지)
  }

  if (device_id.rfind("asio:", 0) == 0) {
    audio_sink_ = MakeElement("asiosink", "asink");
    if (audio_sink_) {
      g_object_set(audio_sink_, "device-clsid", device_id.substr(5).c_str(), "occupy-all-channels",
                   FALSE, nullptr);
    }
  } else {
    audio_sink_ = MakeElement("wasapi2sink", "asink");
    if (audio_sink_ && !device_id.empty()) {
      g_object_set(audio_sink_, "device", device_id.c_str(), nullptr);
    }
  }
  if (!audio_sink_) audio_sink_ = MakeElement("autoaudiosink", "asink");
  gst_bin_add(GST_BIN(pipeline_), audio_sink_);
  const bool ok = gst_element_link(audio_tail_, audio_sink_);
  gst_element_sync_state_with_parent(audio_sink_);

  const std::string label = device_id.empty() ? "default" : device_id;
  const int ch = output_channels_;
  InvokeOnMain([this, ok, label, ch] {
    if (ok) {
      feedback_("info", "audio device applied: " + label + " (" + std::to_string(ch) + "ch bus)");
    } else {
      feedback_("error", "audio device switch failed: " + label);
    }
  });
}

void PlayerCore::ApplyDeckRouting(Deck* deck) {
  if (!deck->amix_pad) return;
  SetPadMatrix(deck->amix_pad, deck->branch_channels, output_channels_, deck->channel_routes);
}

void PlayerCore::SetDeckAudio(const json& msg, int window_id) {
  Surface* s = GetSurface(window_id);
  if (!s || s->live_deck < 0 || !s->decks[s->live_deck]) {
    feedback_("debug", "set_deck_audio: no live deck");
    return;
  }
  Deck* deck = s->decks[s->live_deck].get();

  const json* stream = nullptr;
  if (const auto it = msg.find("streams"); it != msg.end() && it->is_array() && !it->empty()) {
    stream = &(*it)[0];
  }
  if (stream) {
    if (const auto ch = stream->find("channels"); ch != stream->end() && ch->is_array()) {
      deck->channel_routes.clear();
      for (const auto& c : *ch) deck->channel_routes.push_back(ParseChannel(c));
      ApplyDeckRouting(deck);
    }
    if (stream->contains("volume") || stream->contains("muted")) {
      if (const auto v = stream->find("volume"); v != stream->end() && v->is_number())
        deck->volume_gain = std::clamp(v->get<double>(), 0.0, 100.0) / 100.0;
      if (stream->contains("muted")) deck->master_muted = stream->value("muted", false);
      if (deck->audio_tail)
        g_object_set(deck->audio_tail, "volume", deck->master_muted ? 0.0 : deck->volume_gain,
                     nullptr);
    }
    return;
  }
  if (const auto it = msg.find("channel_map"); it != msg.end()) {
    deck->channel_routes.clear();
    if (it->is_array()) {
      for (const auto& v : *it) {
        const int out = v.is_number_integer() ? v.get<int>() : -1;
        deck->channel_routes.push_back({out, 1.0f, out < 0});
      }
    }
    ApplyDeckRouting(deck);
  }
  bool touch_vol = false;
  if (const auto it = msg.find("volume"); it != msg.end() && it->is_number()) {
    deck->volume_gain = std::clamp(it->get<double>(), 0.0, 100.0) / 100.0;
    touch_vol = true;
  }
  if (const auto it = msg.find("muted"); it != msg.end()) {
    deck->master_muted = it->get<bool>();
    touch_vol = true;
  }
  if (touch_vol && deck->audio_tail) {
    g_object_set(deck->audio_tail, "volume", deck->master_muted ? 0.0 : deck->volume_gain, nullptr);
  }
}

// ---------------------------------------------------------------------------
// 레거시 트랙 경로 — 주 창(0)
// ---------------------------------------------------------------------------

void PlayerCore::SetTracks(const json& tracks) {
  tracks_ = tracks.is_array() ? tracks : json::array();
  feedback_("debug", "tracks set: " + std::to_string(tracks_.size()));
}

bool PlayerCore::PlayTrackIndex(int idx) {
  const int n = static_cast<int>(tracks_.size());
  if (n == 0 || idx < 0 || idx >= n) {
    feedback_("error", "playlist_play: index out of range: " + std::to_string(idx));
    return false;
  }
  track_index_ = idx;
  const json& file = tracks_[idx];
  PlayFile(file, idx, file.value("time", 0.0), 0);
  return true;
}

void PlayerCore::Previous() {
  const int n = static_cast<int>(tracks_.size());
  if (n == 0) {
    feedback_("warn", "previous: no tracks set");
    return;
  }
  Surface* s = DefaultSurface();
  const int cur = (s && s->live_deck >= 0 && s->decks[s->live_deck] &&
                   s->decks[s->live_deck]->track_idx >= 0)
                      ? s->decks[s->live_deck]->track_idx
                      : track_index_;
  PlayTrackIndex((cur - 1 + n) % n);
}

// ---------------------------------------------------------------------------
// 독립 오디오 트랙 (v2 §5) — 전역
// ---------------------------------------------------------------------------

void PlayerCore::OnAudioTrackPadAdded(GstElement*, GstPad* pad, gpointer user_data) {
  auto* track = static_cast<AudioTrack*>(user_data);
  PlayerCore* core = track->core;

  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  const bool is_audio = g_str_has_prefix(name, "audio/");
  gst_caps_unref(caps);

  if (!is_audio) {
    GstElement* fake = MakeElement("fakesink", nullptr);
    g_object_set(fake, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add(GST_BIN(track->bin), fake);
    GstPad* fsink = gst_element_get_static_pad(fake, "sink");
    gst_pad_link(pad, fsink);
    gst_object_unref(fsink);
    gst_element_sync_state_with_parent(fake);
    return;
  }
  if (track->out) return;

  GstElement* q = MakeElement("queue", nullptr);
  GstElement* conv = MakeElement("audioconvert", nullptr);
  GstElement* res = MakeElement("audioresample", nullptr);
  GstElement* capsf = MakeElement("capsfilter", nullptr);
  {
    GstCaps* bcaps = MakeBranchCaps(track->branch_channels);
    g_object_set(capsf, "caps", bcaps, nullptr);
    gst_caps_unref(bcaps);
  }
  GstElement* vol = MakeElement("volume", nullptr);
  g_object_set(vol, "volume", track->master_muted ? 0.0 : track->volume_gain, nullptr);

  gst_bin_add_many(GST_BIN(track->bin), q, conv, res, capsf, vol, nullptr);
  bool ok = gst_element_link_many(q, conv, res, capsf, vol, nullptr);
  GstPad* qsink = gst_element_get_static_pad(q, "sink");
  ok = ok && gst_pad_link(pad, qsink) == GST_PAD_LINK_OK;
  gst_object_unref(qsink);

  track->volume_el = vol;
  track->out = gst_element_get_static_pad(vol, "src");

  track->block = gst_pad_add_probe(
      track->out,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
      [](GstPad*, GstPadProbeInfo*, gpointer user_data) -> GstPadProbeReturn {
        static_cast<AudioTrack*>(user_data)->ready = true;
        return GST_PAD_PROBE_OK;
      },
      track, nullptr);

  track->eos_probe = gst_pad_add_probe(
      track->out, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      [](GstPad*, GstPadProbeInfo* info, gpointer user_data) -> GstPadProbeReturn {
        auto* t = static_cast<AudioTrack*>(user_data);
        if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_EOS)
          return GST_PAD_PROBE_OK;
        PlayerCore* c = t->core;
        if (t->loop && t->state == AudioTrack::State::Live) {
          InvokeOnMain([c, id = t->id] {
            auto it = c->audio_tracks_.find(id);
            if (it == c->audio_tracks_.end() || it->second->state != AudioTrack::State::Live)
              return;
            c->LoopAudioTrack(it->second.get());
          });
          return GST_PAD_PROBE_DROP;
        }
        if (!t->stopped_sent) {
          t->stopped_sent = true;
          InvokeOnMain([c, id = t->id] {
            auto it = c->audio_tracks_.find(id);
            if (it != c->audio_tracks_.end()) {
              c->EmitAudioTrackData(it->second.get(), "stopped");
              c->TeardownAudioTrack(id);
            }
          });
        }
        return GST_PAD_PROBE_OK;
      },
      track, nullptr);

  gst_element_sync_state_with_parent(q);
  gst_element_sync_state_with_parent(conv);
  gst_element_sync_state_with_parent(res);
  gst_element_sync_state_with_parent(capsf);
  gst_element_sync_state_with_parent(vol);
  if (!ok) InvokeOnMain([core] { core->feedback_("error", "audio track: branch link failed"); });
}

void PlayerCore::AudioTrackPlay(const json& msg) {
  std::string id;
  if (const auto it = msg.find("track_id"); it != msg.end()) {
    if (it->is_string()) id = it->get<std::string>();
    else if (it->is_number_integer()) id = std::to_string(it->get<long long>());
  }
  if (id.empty()) {
    feedback_("error", "audio_track_play: track_id missing");
    return;
  }
  if (!msg.contains("file") || !msg["file"].is_object()) {
    feedback_("error", "audio_track_play: file missing");
    return;
  }
  const json& file = msg["file"];
  auto uri = ToUri(file.value("path", ""));
  if (!uri) {
    feedback_("error", "audio_track_play: invalid media path: " + file.value("path", ""));
    return;
  }

  if (audio_tracks_.count(id)) TeardownAudioTrack(id);
  if (audio_tracks_.size() >= kMaxAudioTracks) {
    feedback_("error",
              "audio_track_play: too many tracks (max " + std::to_string(kMaxAudioTracks) + ")");
    return;
  }

  auto track = std::make_unique<AudioTrack>();
  track->core = this;
  track->id = id;
  track->file = file;
  track->loop = msg.value("loop", false);
  track->delay_ms = std::max<gint64>(0, msg.value("delay_ms", static_cast<int64_t>(0)));
  track->in_ms = msg.contains("in_ms") ? msg.value("in_ms", static_cast<gint64>(0))
                                       : file.value("in_ms", static_cast<gint64>(0));
  const json* chans = nullptr;
  if (const auto it = msg.find("channels"); it != msg.end() && it->is_array() && !it->empty())
    chans = &*it;
  else if (const auto fit = file.find("channels");
           fit != file.end() && fit->is_array() && !fit->empty())
    chans = &*fit;
  if (chans) {
    for (const auto& c : *chans) track->channel_routes.push_back(ParseChannel(c));
  } else {
    const json* map_src = nullptr;
    if (const auto it = msg.find("channel_map"); it != msg.end() && it->is_array() && !it->empty())
      map_src = &*it;
    else if (const auto fit = file.find("channel_map");
             fit != file.end() && fit->is_array() && !fit->empty())
      map_src = &*fit;
    if (map_src) {
      for (const auto& v : *map_src) {
        const int out = v.is_number_integer() ? v.get<int>() : -1;
        track->channel_routes.push_back({out, 1.0f, out < 0});
      }
    }
  }
  if (!track->channel_routes.empty())
    track->branch_channels = static_cast<int>(track->channel_routes.size());
  if (const auto it = msg.find("volume"); it != msg.end() && it->is_number())
    track->volume_gain = std::clamp(it->get<double>(), 0.0, 100.0) / 100.0;
  else if (const auto fit = file.find("volume"); fit != file.end() && fit->is_number())
    track->volume_gain = std::clamp(fit->get<double>(), 0.0, 100.0) / 100.0;
  track->master_muted = msg.value("muted", false) || file.value("muted", false);

  track->bin = gst_bin_new(("atrack_" + id).c_str());
  track->decode = MakeElement("uridecodebin3", nullptr);
  g_object_set(track->decode, "uri", uri->c_str(), nullptr);
  g_signal_connect(track->decode, "pad-added", G_CALLBACK(OnAudioTrackPadAdded), track.get());
  gst_bin_add(GST_BIN(track->bin), track->decode);

  gst_bin_add(GST_BIN(pipeline_), track->bin);
  gst_element_set_locked_state(track->bin, TRUE);
  gst_element_set_state(track->bin, GST_STATE_PAUSED);

  track->preroll_started = gst_clock_get_time(gst_system_clock_obtain());
  AudioTrack* raw = track.get();
  track->preroll_watch = g_timeout_add(50, [](gpointer data) -> gboolean {
    auto* t = static_cast<AudioTrack*>(data);
    if (t->core->CheckAudioTrackPreroll(t)) return G_SOURCE_CONTINUE;
    t->preroll_watch = 0;
    return G_SOURCE_REMOVE;
  }, raw);

  audio_tracks_[id] = std::move(track);
}

bool PlayerCore::CheckAudioTrackPreroll(AudioTrack* track) {
  GstState state = GST_STATE_NULL;
  if (gst_element_get_state(track->bin, &state, nullptr, 0) == GST_STATE_CHANGE_FAILURE) {
    feedback_("error", "audio track preroll failed: " + track->file.value("path", ""));
    EmitAudioTrackData(track, "stopped");
    TeardownAudioTrack(track->id);
    return false;
  }
  if (track->ready) {
    if (track->in_ms > 0 && !track->inpoint_seeked) {
      track->inpoint_seeked = true;
      track->ready = false;
      GstEvent* seek = gst_event_new_seek(
          1.0, GST_FORMAT_TIME,
          static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
          GST_SEEK_TYPE_SET, track->in_ms * GST_MSECOND, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
      gst_pad_send_event(track->out, seek);
      track->preroll_started = gst_clock_get_time(gst_system_clock_obtain());
      return true;
    }
    // 오디오 트랙별 시작 지연: 프리롤 완료 후 delay_ms 대기했다 amix 연결
    if (track->delay_ms > 0 && !track->delay_timer) {
      track->delay_timer = g_timeout_add(
          static_cast<guint>(track->delay_ms),
          [](gpointer data) -> gboolean {
            auto* t = static_cast<AudioTrack*>(data);
            t->delay_timer = 0;
            if (t->state == AudioTrack::State::Building) t->core->ConnectAudioTrack(t);
            return G_SOURCE_REMOVE;
          },
          track);
      return false;  // 폴링 종료 — 타이머가 연결 담당
    }
    ConnectAudioTrack(track);
    return false;
  }
  const GstClockTime now = gst_clock_get_time(gst_system_clock_obtain());
  if (now - track->preroll_started > kPrerollTimeout) {
    feedback_("error", "audio track preroll timeout: " + track->file.value("path", ""));
    EmitAudioTrackData(track, "stopped");
    TeardownAudioTrack(track->id);
    return false;
  }
  return true;
}

void PlayerCore::ConnectAudioTrack(AudioTrack* track) {
  track->ghost = gst_ghost_pad_new("audio_src", track->out);
  gst_pad_set_active(track->ghost, TRUE);
  gst_element_add_pad(track->bin, track->ghost);

  track->amix_pad = gst_element_request_pad_simple(amix_, "sink_%u");
  SetPadMatrix(track->amix_pad, track->branch_channels, output_channels_, track->channel_routes);
  gst_pad_set_offset(track->out, static_cast<gint64>(RunningTime()));
  if (gst_pad_link(track->ghost, track->amix_pad) != GST_PAD_LINK_OK) {
    feedback_("error", "audio track: link to mixer failed");
  }

  gst_element_set_locked_state(track->bin, FALSE);
  gst_element_sync_state_with_parent(track->bin);
  if (track->block) {
    gst_pad_remove_probe(track->out, track->block);
    track->block = 0;
  }
  track->state = AudioTrack::State::Live;
  EmitAudioTrackData(track, "playing");
  feedback_("debug", "audio track started: " + track->id);
}

void PlayerCore::LoopAudioTrack(AudioTrack* track) {
  GstEvent* seek = gst_event_new_seek(
      1.0, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
      GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  if (!gst_pad_send_event(track->out, seek)) {
    feedback_("warn", "audio track loop seek failed: " + track->id);
    return;
  }
  gst_pad_set_offset(track->out, static_cast<gint64>(RunningTime()));
}

void PlayerCore::AudioTrackStop(const std::string& track_id) {
  auto it = audio_tracks_.find(track_id);
  if (it == audio_tracks_.end()) {
    feedback_("debug", "audio_track_stop: unknown track: " + track_id);
    return;
  }
  EmitAudioTrackData(it->second.get(), "stopped");
  TeardownAudioTrack(track_id);
}

void PlayerCore::StopAllAudioTracks() {
  std::vector<std::string> ids;
  for (const auto& [id, t] : audio_tracks_) ids.push_back(id);
  for (const auto& id : ids) AudioTrackStop(id);
}

void PlayerCore::AudioTrackPause(const std::string& track_id) {
  auto it = audio_tracks_.find(track_id);
  if (it == audio_tracks_.end() || it->second->state != AudioTrack::State::Live) return;
  AudioTrack* track = it->second.get();
  if (!track->paused) {
    track->paused_running = RunningTime();
    if (!track->block) {
      track->block = gst_pad_add_probe(
          track->out,
          static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
          nullptr, nullptr, nullptr);
    }
    track->paused = true;
    EmitAudioTrackData(track, "paused");
  } else {
    const GstClockTime delta = RunningTime() - track->paused_running;
    gst_pad_set_offset(track->out, gst_pad_get_offset(track->out) + static_cast<gint64>(delta));
    if (track->block) {
      gst_pad_remove_probe(track->out, track->block);
      track->block = 0;
    }
    track->paused = false;
    EmitAudioTrackData(track, "playing");
  }
}

void PlayerCore::AudioTrackSetVolume(const std::string& track_id, double volume) {
  auto it = audio_tracks_.find(track_id);
  if (it == audio_tracks_.end()) return;
  AudioTrack* track = it->second.get();
  track->volume_gain = std::clamp(volume, 0.0, 100.0) / 100.0;
  if (track->volume_el) g_object_set(track->volume_el, "volume", track->volume_gain, nullptr);
}

void PlayerCore::AudioTrackSetChannelMap(const std::string& track_id, const json& arg) {
  auto it = audio_tracks_.find(track_id);
  if (it == audio_tracks_.end() || !arg.is_array()) return;
  AudioTrack* track = it->second.get();
  track->channel_routes.clear();
  for (const auto& c : arg) {
    if (c.is_object()) {
      track->channel_routes.push_back(ParseChannel(c));
    } else {
      const int out = c.is_number_integer() ? c.get<int>() : -1;
      track->channel_routes.push_back({out, 1.0f, out < 0});
    }
  }
  if (track->amix_pad) {
    SetPadMatrix(track->amix_pad, track->branch_channels, output_channels_, track->channel_routes);
  }
}

void PlayerCore::TeardownAudioTrack(const std::string& track_id) {
  auto it = audio_tracks_.find(track_id);
  if (it == audio_tracks_.end()) return;
  AudioTrack* track = it->second.get();
  track->state = AudioTrack::State::Dead;
  if (track->preroll_watch) {
    g_source_remove(track->preroll_watch);
    track->preroll_watch = 0;
  }
  if (track->delay_timer) {
    g_source_remove(track->delay_timer);
    track->delay_timer = 0;
  }
  if (track->block) {
    gst_pad_remove_probe(track->out, track->block);
    track->block = 0;
  }
  if (track->eos_probe) {
    gst_pad_remove_probe(track->out, track->eos_probe);
    track->eos_probe = 0;
  }
  gst_element_set_locked_state(track->bin, TRUE);
  gst_element_set_state(track->bin, GST_STATE_NULL);
  if (track->amix_pad) {
    if (track->ghost) gst_pad_unlink(track->ghost, track->amix_pad);
    gst_element_release_request_pad(amix_, track->amix_pad);
    gst_object_unref(track->amix_pad);
  }
  if (track->out) gst_object_unref(track->out);
  gst_bin_remove(GST_BIN(pipeline_), track->bin);
  audio_tracks_.erase(it);
}

void PlayerCore::EmitAudioTrackData(AudioTrack* track, const char* state) {
  gint64 pos_ns = -1, dur_ns = -1;
  if (track->out) {
    gst_pad_query_position(track->out, GST_FORMAT_TIME, &pos_ns);
    gst_pad_query_duration(track->out, GST_FORMAT_TIME, &dur_ns);
  }
  const gint64 time_ms = pos_ns >= 0 ? pos_ns / GST_MSECOND : 0;
  const gint64 dur_ms = dur_ns >= 0 ? dur_ns / GST_MSECOND : 0;
  const bool playing = g_strcmp0(state, "playing") == 0;
  feedback_("audio_track_data",
            json{{"track_id", track->id},
                 {"time", time_ms},
                 {"duration", dur_ms},
                 {"position", dur_ms > 0 ? static_cast<double>(time_ms) / dur_ms : 0.0},
                 {"is_playing", playing},
                 {"state", state}});
}

void PlayerCore::EmitTick() {
  if (timeline_active_) {
    TimelineTick();
    return;
  }

  for (auto& [id, track] : audio_tracks_) {
    if (track && track->state == AudioTrack::State::Live) {
      EmitAudioTrackData(track.get(), track->paused ? "paused" : "playing");
    }
  }

  for (auto& [wid, s] : surfaces_) {
    if (s->live_deck < 0 || !s->decks[s->live_deck]) continue;
    Deck* deck = s->decks[s->live_deck].get();
    GstPad* pad = deck->video_out ? deck->video_out : deck->audio_out;
    if (!pad) continue;

    gint64 time_ms = 0, dur_ms = 0;
    if (deck->is_image) {
      time_ms = deck->image_elapsed_ms +
                (s->paused ? 0
                           : static_cast<gint64>((RunningTime() - deck->image_started) /
                                                 GST_MSECOND));
      dur_ms = deck->image_time_ms;
      if (dur_ms > 0 && time_ms > dur_ms) time_ms = dur_ms;
    } else {
      gint64 pos_ns = -1, dur_ns = -1;
      gst_pad_query_position(pad, GST_FORMAT_TIME, &pos_ns);
      gst_pad_query_duration(pad, GST_FORMAT_TIME, &dur_ns);
      time_ms = pos_ns >= 0 ? pos_ns / GST_MSECOND : 0;
      dur_ms = dur_ns >= 0 ? dur_ns / GST_MSECOND : 0;
    }
    const double position = dur_ms > 0 ? static_cast<double>(time_ms) / dur_ms : 0.0;
    const bool playing = !s->paused;

    feedback_("player_data", json{{"id", deck->id},
                                  {"window_id", wid},
                                  {"event", playing ? "playing" : "paused"},
                                  {"time", time_ms},
                                  {"duration", dur_ms},
                                  {"position", position},
                                  {"is_playing", playing},
                                  {"state", playing ? "playing" : "paused"}});
  }
}

}  // namespace vp
