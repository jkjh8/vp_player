// vplayer — 네트워크 제어 미디어 플레이어 (C++20 + GStreamer)
//
// 구성:
//   - 자체 Win32 창 (video_window) + d3d11videosink GstVideoOverlay
//   - 듀얼 덱 + d3d11compositor/audiomixer (player_core) — 갭리스 전환
//   - 루프백 NDJSON TCP 서버 + stdout 포트 핸드셰이크 (net/tcp_server)
// 프로토콜 계약: docs/PROTOCOL.md

#include <windows.h>
#include <psapi.h>

#include <gst/gst.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "media/probe.h"
#include "net/tcp_server.h"
#include "player/player_core.h"
#include "video/display_enum.h"
#include "video/tray_icon.h"
#include "video/video_window.h"

using json = nlohmann::json;

namespace {

struct App {
  GMainLoop* loop = nullptr;
  vp::NdjsonServer server;
  vp::PlayerCore core;  // 창(Surface)은 core가 소유 — 동적 생성/삭제 (자동 주 창 없음)
  vp::TrayIcon tray;    // 실행 표시 + 종료
};

App* g_app = nullptr;

void SendFeedback(const std::string& type, const json& data) {
  json msg = {{"type", type}, {"data", data}};
  g_app->server.SendLine(msg.dump());
}

// window_id 주소. 명시되면 그 값, 없으면 기본 창(존재하는 첫 창) — 주 창 개념 폐지.
int WindowIdOf(const json& msg) {
  return msg.contains("window_id") ? msg.value("window_id", 0) : g_app->core.DefaultWindowId();
}

// {monitor_index,x,y,width,height} → WindowPlacement
vp::WindowPlacement PlacementFromJson(const json& msg) {
  vp::WindowPlacement p;
  p.monitor_index = msg.value("monitor_index", -1);
  p.x = msg.value("x", 0);
  p.y = msg.value("y", 0);
  p.width = msg.value("width", 0);
  p.height = msg.value("height", 0);
  return p;
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

  const int wid = WindowIdOf(msg);

  if (cmd == "create_window") {
    std::string aspect = msg.value("aspect_mode", std::string("letterbox"));
    const bool ok = core.CreateSurface(wid, PlacementFromJson(msg), aspect);
    SendFeedback("windows", json{{"windows", core.ListSurfaces()}, {"created", wid}, {"ok", ok}});
  } else if (cmd == "destroy_window") {
    core.DestroySurface(wid);
    SendFeedback("windows", json{{"windows", core.ListSurfaces()}, {"destroyed", wid}});
  } else if (cmd == "get_windows") {
    SendFeedback("windows", json{{"windows", core.ListSurfaces()}});
  } else if (cmd == "set_preload_config") {
    core.SetPreloadConfig(msg);
  } else if (cmd == "preload_playlist") {
    core.PreloadPlaylist(msg);
  } else if (cmd == "enable_ptp") {
    core.EnablePtp(msg.value("domain", 0));
  } else if (cmd == "ptp_base_time") {
    core.SetPtpBaseTime(msg.value("base_time", static_cast<int64_t>(0)));
  } else if (cmd == "get_running_time") {
    SendFeedback("running_time", core.PtpStatus());
  } else if (cmd == "playid" || cmd == "set_media") {
    if (!msg.contains("file")) {
      SendFeedback("error", cmd + ": file missing");
      return;
    }
    // 이미지 표시 시간: file.time (초, 0 = 무한) — 프로토콜 §file 객체
    const double image_time = msg["file"].value("time", 0.0);
    core.PlayFile(msg["file"], msg.value("track_idx", -1), image_time, wid);
  } else if (cmd == "play_current_and_load_next") {
    const int track_idx = msg.value("track_idx", -1);
    int current_slot = -1;
    if (msg.contains("current") && !msg["current"].is_null()) {
      // current_time (초) 이 file.time 을 덮어씀 — 프로토콜 §2.5
      const double t = msg.value("current_time", msg["current"].value("time", 0.0));
      current_slot = core.PlayFile(msg["current"], track_idx, t, wid);
    }
    if (msg.contains("next") && !msg["next"].is_null()) {
      const double t = msg.value("next_time", msg["next"].value("time", 0.0));
      core.PreloadNext(msg["next"], track_idx >= 0 ? track_idx + 1 : -1, t, current_slot, wid);
    }
  } else if (cmd == "preload_next") {
    if (msg.contains("next") && !msg["next"].is_null()) {
      const double t = msg.value("next_time", msg["next"].value("time", 0.0));
      core.PreloadNext(msg["next"], msg.value("next_track_idx", -1), t, -1, wid);
    }
  } else if (cmd == "next") {
    core.Next(wid);
  } else if (cmd == "previous") {
    core.Previous();
  } else if (cmd == "play") {
    core.Play(wid);
  } else if (cmd == "pause") {
    core.Pause(wid);
  } else if (cmd == "stop" || cmd == "stop_all") {
    // host(pStatus.activePlayerId)가 필터링하는 id는 "방금까지 활성이던 덱"이어야 매칭된다 —
    // Stop()이 live_deck를 -1로 리셋하므로 호출 전에 캡처해둔다.
    const int active_id = std::max(0, core.ActiveDeckId(wid));
    if (cmd == "stop_all") core.StopAll();
    else core.Stop(wid);
    SendFeedback("player_data", json{{"id", active_id},
                                     {"window_id", wid},
                                     {"event", "stopped"},
                                     {"time", 0},
                                     {"duration", 0},
                                     {"position", 0.0},
                                     {"is_playing", false},
                                     {"state", "stopped"}});
  } else if (cmd == "set_time") {
    core.SeekMs(msg.value("time", static_cast<int64_t>(0)), wid);
  } else if (cmd == "set_fullscreen") {
    const bool value = msg.value("value", false);
    core.SetFullscreen(value, wid);
    SendFeedback("set_fullscreen", json{{"window_id", wid}, {"value", value}});
  } else if (cmd == "background_color") {
    uint32_t rgb = 0;
    if (ParseHexColor(msg.value("color", ""), &rgb)) {
      core.SetBackgroundColor(rgb, wid);
      SendFeedback("set_background", msg.value("color", ""));
    } else {
      SendFeedback("error", "background_color: invalid color: " + msg.value("color", ""));
    }
  } else if (cmd == "get_displays") {
    json displays = json::array();
    for (const auto& m : vp::EnumerateMonitors()) {
      displays.push_back({{"index", m.index},
                          {"device_name", m.device_name},
                          {"x", m.x},
                          {"y", m.y},
                          {"width", m.width},
                          {"height", m.height},
                          {"primary", m.primary}});
    }
    SendFeedback("displays", json{{"displays", displays}});
  } else if (cmd == "set_display") {
    vp::WindowPlacement p = PlacementFromJson(msg);
    core.ApplyDisplayPlacement(p, wid);

    // ApplyPlacement는 창 스레드로 비동기 마샬링되므로 client_*()를 바로 읽으면 레이스가
    // 생긴다 — aspect-ratio 계산에 쓸 목표 크기는 여기서 직접 재계산한다.
    int target_w = p.width, target_h = p.height;
    if (target_w <= 0 || target_h <= 0) {
      for (const auto& m : vp::EnumerateMonitors()) {
        if (m.index == p.monitor_index || (p.monitor_index < 0 && m.primary)) {
          if (target_w <= 0) target_w = m.width;
          if (target_h <= 0) target_h = m.height;
          break;
        }
      }
    }
    const std::string aspect_mode = msg.value("aspect_mode", std::string("letterbox"));
    core.SetAspectMode(aspect_mode, target_w, target_h, wid);
    SendFeedback("set_display", msg);
  } else if (cmd == "get_audio_devices") {
    SendFeedback("audiodevices", json{{"devices", core.ListAudioDevices()}});
  } else if (cmd == "get_audio_device_caps") {
    // v2 (§5): v1 audiodevices와 동일 데이터 — 열람이 이미 type/channels를 포함
    SendFeedback("audio_device_caps", json{{"devices", core.ListAudioDevices()}});
  } else if (cmd == "set_audio_device") {
    core.SetAudioDevice(msg.value("device_id", ""));
  } else if (cmd == "set_deck_audio") {
    // 활성 덱(임베디드 오디오) 라이브 라우팅/볼륨/뮤트
    core.SetDeckAudio(msg, wid);
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
  } else if (cmd == "set_timeline") {
    // v2 §5.2: 타임라인 시트 로드 — {timeline_id, duration_ms, tracks:[...]}
    core.SetTimeline(msg);
  } else if (cmd == "timeline_play") {
    core.TimelinePlay(msg.value("time_ms", (int64_t)0), msg.contains("time_ms"));
  } else if (cmd == "timeline_pause") {
    core.TimelinePause();
  } else if (cmd == "timeline_seek") {
    core.TimelineSeek(msg.value("time_ms", (int64_t)0));
  } else if (cmd == "timeline_stop") {
    core.TimelineStop();
  } else if (cmd == "show_logo") {
    core.SetLogoEnabled(msg.value("show", false), wid);
  } else if (cmd == "logo_file") {
    core.SetLogoFile(msg.value("file", ""), wid);
  } else if (cmd == "logo_size") {
    core.SetLogoSize(msg.value("size", 0), wid);
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

// 메모리 상태 주기 발신 (1s): 프로세스 RSS + 시스템 가용/총 메모리 + 엔진 통계.
// 호스트(vp_app2)가 pStatus.memory로 병합해 UI에 표시 (전 트랙 프리롤 압박 가시화).
gboolean OnMemoryTick(gpointer) {
  json data = g_app->core.EngineStats();

  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    data["rss_bytes"] = static_cast<uint64_t>(pmc.WorkingSetSize);
    data["private_bytes"] = static_cast<uint64_t>(pmc.PrivateUsage);
  }
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) {
    data["sys_total_bytes"] = static_cast<uint64_t>(ms.ullTotalPhys);
    data["sys_avail_bytes"] = static_cast<uint64_t>(ms.ullAvailPhys);
    data["sys_load_percent"] = static_cast<int>(ms.dwMemoryLoad);
  }
  SendFeedback("memory_status", data);
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
               json{{"features",
                     json::array({"channel_map", "audio_track", "live_routing", "embedded_streams",
                                  "display", "timeline", "multi_window", "track_delay",
                                  "memory_status", "ptp_sync"})}});
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

  // 공유 파이프라인 + 전역 오디오 버스 구성. 창은 호스트가 create_window로 필요할 때 생성
  // (자동 주 창 없음 — 사용자가 설정한 창만 열린다).
  if (!app.core.Init(SendFeedback)) {
    fprintf(stderr, "FATAL: player core init failed\n");
    return 1;
  }

  // 창 닫힘: 해당 창만 해체하고 통지 (주 창 개념 없음 — 창을 닫아도 프로그램은 유지,
  // 종료는 트레이 메뉴로). 사용자가 실수로 창을 닫아도 앱은 계속 실행.
  app.core.SetSurfaceClosedHandler([](int window_id) {
    g_app->core.DestroySurface(window_id);
    SendFeedback("windows",
                 json{{"windows", g_app->core.ListSurfaces()}, {"destroyed", window_id}});
  });

  // 트레이 아이콘 — 실행 표시 + 종료 메뉴 (창이 없어도 프로그램 제어 가능)
  app.tray.Create(L"VP App — 실행 중", [] {
    g_main_context_invoke(
        nullptr,
        [](gpointer) -> gboolean {
          SendFeedback("closed", nullptr);
          g_main_loop_quit(g_app->loop);
          return G_SOURCE_REMOVE;
        },
        nullptr);
  });

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
  g_timeout_add(1000, OnMemoryTick, nullptr);
  g_timeout_add(500, SendReady, nullptr);

  g_main_loop_run(app.loop);

  app.server.Stop();
  app.tray.Destroy();
  app.core.Shutdown();  // 전 창(창 스레드 join 포함) + 파이프라인 해체
  g_main_loop_unref(app.loop);
  return 0;
}
