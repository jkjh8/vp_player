// vplayer — 네트워크 제어 미디어 플레이어 (C++20 + GStreamer)
//
// 구성:
//   - 자체 Win32 창 (video_window) + d3d11videosink GstVideoOverlay
//   - 듀얼 덱 + d3d11compositor/audiomixer (player_core) — 갭리스 전환
//   - 루프백 NDJSON TCP 서버 + stdout 포트 핸드셰이크 (net/tcp_server)
// 프로토콜 계약: docs/PROTOCOL.md

#include <windows.h>

#include <gst/gst.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "media/probe.h"
#include "net/tcp_server.h"
#include "player/player_core.h"
#include "video/video_window.h"

using json = nlohmann::json;

namespace {

struct App {
  GMainLoop* loop = nullptr;
  vp::NdjsonServer server;
  vp::VideoWindow window;
  vp::PlayerCore core;
};

App* g_app = nullptr;

void SendFeedback(const std::string& type, const json& data) {
  json msg = {{"type", type}, {"data", data}};
  g_app->server.SendLine(msg.dump());
}

// track_id는 문자열이 규약이나 숫자도 허용 (json.value는 타입 불일치 시 throw)
std::string TrackIdOf(const json& msg) {
  const auto it = msg.find("track_id");
  if (it == msg.end()) return "";
  if (it->is_string()) return it->get<std::string>();
  if (it->is_number_integer()) return std::to_string(it->get<long long>());
  return "";
}

// "#RRGGBB" / "RRGGBB" → 0xRRGGBB
bool ParseHexColor(const std::string& text, uint32_t* out) {
  std::string hex = text;
  if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
  if (hex.size() != 6) return false;
  *out = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
  return true;
}

// ---------- 명령 디스패치 (GLib 메인루프 스레드) ----------

void HandleCommand(const json& msg) {
  const std::string cmd = msg.value("command", "");
  auto& core = g_app->core;

  if (cmd == "playid" || cmd == "set_media") {
    if (!msg.contains("file")) {
      SendFeedback("error", cmd + ": file missing");
      return;
    }
    // 이미지 표시 시간: file.time (초, 0 = 무한) — 프로토콜 §file 객체
    const double image_time = msg["file"].value("time", 0.0);
    core.PlayFile(msg["file"], msg.value("track_idx", -1), image_time);
  } else if (cmd == "play_current_and_load_next") {
    const int track_idx = msg.value("track_idx", -1);
    int current_slot = -1;
    if (msg.contains("current") && !msg["current"].is_null()) {
      // current_time (초) 이 file.time 을 덮어씀 — 프로토콜 §2.5
      const double t = msg.value("current_time", msg["current"].value("time", 0.0));
      current_slot = core.PlayFile(msg["current"], track_idx, t);
    }
    if (msg.contains("next") && !msg["next"].is_null()) {
      const double t = msg.value("next_time", msg["next"].value("time", 0.0));
      core.PreloadNext(msg["next"], track_idx >= 0 ? track_idx + 1 : -1, t, current_slot);
    }
  } else if (cmd == "preload_next") {
    if (msg.contains("next") && !msg["next"].is_null()) {
      const double t = msg.value("next_time", msg["next"].value("time", 0.0));
      core.PreloadNext(msg["next"], msg.value("next_track_idx", -1), t);
    }
  } else if (cmd == "next") {
    core.Next();
  } else if (cmd == "previous") {
    core.Previous();
  } else if (cmd == "play") {
    core.Play();
  } else if (cmd == "pause") {
    core.Pause();
  } else if (cmd == "stop" || cmd == "stop_all") {
    core.Stop();
    SendFeedback("player_data", json{{"id", 0},
                                     {"event", "stopped"},
                                     {"time", 0},
                                     {"duration", 0},
                                     {"position", 0.0},
                                     {"is_playing", false},
                                     {"state", "stopped"}});
  } else if (cmd == "set_time") {
    core.SeekMs(msg.value("time", static_cast<int64_t>(0)));
  } else if (cmd == "set_fullscreen") {
    const bool value = msg.value("value", false);
    g_app->window.SetFullscreen(value);
    SendFeedback("set_fullscreen", value);
  } else if (cmd == "background_color") {
    uint32_t rgb = 0;
    if (ParseHexColor(msg.value("color", ""), &rgb)) {
      g_app->core.SetBackgroundColor(rgb);
      g_app->window.SetBackgroundColor(rgb);
      SendFeedback("set_background", msg.value("color", ""));
    } else {
      SendFeedback("error", "background_color: invalid color: " + msg.value("color", ""));
    }
  } else if (cmd == "get_audio_devices") {
    SendFeedback("audiodevices", json{{"devices", core.ListAudioDevices()}});
  } else if (cmd == "get_audio_device_caps") {
    // v2 (§5): v1 audiodevices와 동일 데이터 — 열람이 이미 type/channels를 포함
    SendFeedback("audio_device_caps", json{{"devices", core.ListAudioDevices()}});
  } else if (cmd == "set_audio_device") {
    core.SetAudioDevice(msg.value("device_id", ""));
  } else if (cmd == "set_deck_audio") {
    // 활성 덱(임베디드 오디오) 라이브 라우팅/볼륨/뮤트
    core.SetDeckAudio(msg);
  } else if (cmd == "playlist_mode") {
    core.SetPlaylistMode(msg.value("value", false));
    SendFeedback("debug", "ack: playlist_mode");
  } else if (cmd == "set_tracks") {
    core.SetTracks(msg.value("tracks", json::array()));
  } else if (cmd == "set_track_index") {
    core.SetTrackIndex(msg.value("index", 0));
    SendFeedback("debug", "ack: set_track_index");
  } else if (cmd == "playlist_play") {
    core.PlayTrackIndex(msg.value("idx", 0));
  } else if (cmd == "probe_media") {
    // ffmpeg-static 대체 (Phase 2.5) — 워커 스레드에서 GstDiscoverer, req_id로 응답 상관
    const int req_id = msg.value("req_id", -1);
    const std::string p = msg.value("path", "");
    vp::ProbeMediaAsync(p, [req_id](const json& meta) {
      json data = meta;
      data["req_id"] = req_id;
      data["ok"] = !meta.contains("error");
      SendFeedback("probe_result", data);
    });
  } else if (cmd == "make_thumbnail") {
    const int req_id = msg.value("req_id", -1);
    const std::string p = msg.value("path", "");
    const std::string outp = msg.value("out", "");
    const bool is_image = msg.value("is_image", false);
    const double at_sec = msg.value("at_sec", 5.0);
    const int width = msg.value("width", 320);
    vp::MakeThumbnailAsync(p, outp, is_image, at_sec, width,
                           [req_id, outp](bool ok, const std::string& err) {
                             SendFeedback("thumbnail_result",
                                          json{{"req_id", req_id},
                                               {"ok", ok},
                                               {"out", outp},
                                               {"error", err}});
                           });
  } else if (cmd == "audio_track_play") {
    // v2 §5: 독립 오디오 트랙 — {track_id, file, volume?, channel_map?, loop?}
    core.AudioTrackPlay(msg);
  } else if (cmd == "audio_track_stop") {
    core.AudioTrackStop(TrackIdOf(msg));
  } else if (cmd == "audio_track_pause") {
    core.AudioTrackPause(TrackIdOf(msg));
  } else if (cmd == "audio_track_set_volume") {
    core.AudioTrackSetVolume(TrackIdOf(msg), msg.value("volume", 100.0));
  } else if (cmd == "audio_track_set_channel_map") {
    core.AudioTrackSetChannelMap(TrackIdOf(msg), msg.value("map", json::array()));
  } else if (cmd == "show_logo") {
    core.SetLogoEnabled(msg.value("show", false));
  } else if (cmd == "logo_file") {
    core.SetLogoFile(msg.value("file", ""));
  } else if (cmd == "logo_size") {
    core.SetLogoSize(msg.value("size", 0));
  } else {
    SendFeedback("debug", "unhandled command: " + cmd);
  }
}

gboolean DispatchLineIdle(gpointer data) {
  auto* line = static_cast<std::string*>(data);
  try {
    HandleCommand(json::parse(*line));
  } catch (const std::exception& e) {
    SendFeedback("error", std::string("bad command json: ") + e.what());
  }
  delete line;
  return G_SOURCE_REMOVE;
}

gboolean OnTick(gpointer) {
  g_app->core.EmitTick();
  return G_SOURCE_CONTINUE;
}

gboolean SendReady(gpointer) {
  // 클라이언트 연결 전 발송은 유실됨 (SendLine은 미연결 시 무시) — 연결될 때까지
  // 500ms 주기로 재시도. PROTOCOL.md §4가 경고한 ready 레이스의 실제 해소 지점.
  if (!g_app->server.HasClient()) return G_SOURCE_CONTINUE;
  SendFeedback("info", "Player ready");
  // v2 기능 협상 (§5): 호스트는 이 목록으로 신규 명령 송신을 게이트 — 구버전 조합에서도
  // 안전하게 강하 (v1 호스트는 모르는 피드백 type을 경고 후 무시)
  SendFeedback("capabilities",
               json{{"features", json::array({"channel_map", "audio_track", "live_routing",
                                              "embedded_streams"})}});
  return G_SOURCE_REMOVE;
}

// ---------- 번들 GStreamer 격리 (배포 시) ----------

void ConfigureBundledGStreamer() {
  // 번들 레이아웃: <exe폴더>\*.dll (코어 라이브러리) + <exe폴더>\gst-plugins\*.dll
  // 코어 DLL은 exe 옆이라 로더가 자동 탐색 — 플러그인 경로/레지스트리만 격리하면 됨
  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  const auto plugins = std::filesystem::path(exe_path).parent_path() / L"gst-plugins";
  if (!std::filesystem::exists(plugins)) return;  // 개발 환경: 시스템 GStreamer 사용

  _wputenv_s(L"GST_PLUGIN_PATH", plugins.c_str());
  _wputenv_s(L"GST_PLUGIN_SYSTEM_PATH_1_0", L"");
  wchar_t local_appdata[MAX_PATH];
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH) > 0) {
    const auto registry = std::filesystem::path(local_appdata) / L"vpapp" / L"gst-registry.bin";
    std::filesystem::create_directories(registry.parent_path());
    _wputenv_s(L"GST_REGISTRY_1_0", registry.c_str());
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  ConfigureBundledGStreamer();
  gst_init(&argc, &argv);

  App app;
  g_app = &app;
  app.loop = g_main_loop_new(nullptr, FALSE);

  // 비디오 창 (닫으면 closed 통지 후 종료 — 호스트가 app.quit()으로 이어감)
  if (!app.window.Create(1280, 720, [] {
        g_main_context_invoke(
            nullptr,
            [](gpointer) -> gboolean {
              SendFeedback("closed", nullptr);
              g_main_loop_quit(g_app->loop);
              return G_SOURCE_REMOVE;
            },
            nullptr);
      })) {
    fprintf(stderr, "FATAL: cannot create video window\n");
    return 1;
  }

  if (!app.core.Init(app.window.hwnd(), SendFeedback)) {
    fprintf(stderr, "FATAL: player core init failed\n");
    return 1;
  }

  const uint16_t port = app.server.Start(1300, [](std::string line) {
    g_main_context_invoke(nullptr, DispatchLineIdle, new std::string(std::move(line)));
  });
  if (port == 0) {
    fprintf(stderr, "FATAL: cannot bind control socket\n");
    return 1;
  }

  // stdout 계약: 포트 핸드셰이크 딱 한 줄, 단일 flush. 이후 stdout 사용 금지.
  printf("{\"type\": \"port\", \"data\": {\"port\": %u}}\n", port);
  fflush(stdout);

  g_timeout_add(100, OnTick, nullptr);
  g_timeout_add(500, SendReady, nullptr);

  g_main_loop_run(app.loop);

  app.server.Stop();
  app.core.Shutdown();
  app.window.Destroy();
  g_main_loop_unref(app.loop);
  return 0;
}
