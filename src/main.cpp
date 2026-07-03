// vplayer — 네트워크 제어 미디어 플레이어 (C++20 + GStreamer)
//
// 현 단계 = 스파이크 골격:
//   - 루프백 NDJSON TCP 서버 + stdout 포트 핸드셰이크 (프로토콜 전송 계층 완성형)
//   - playbin3 단일 파이프라인 재생/일시정지/정지/시크 (덱 A/B는 다음 단계에서 대체)
//   - 100ms player_data 틱, 오디오 디바이스 열람
// 프로토콜 계약: docs/PROTOCOL.md

#include <windows.h>

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "net/tcp_server.h"

using json = nlohmann::json;

namespace {

struct App {
  GMainLoop* loop = nullptr;
  GstElement* playbin = nullptr;  // 스파이크: 단일 playbin3. 이후 덱 A/B + compositor로 교체.
  vp::NdjsonServer server;

  // 현재 미디어 정보 (media_changed / end_reached 피드백용)
  json current_file;                 // 호스트가 준 file 객체 그대로 보관
  int playlist_track_index = -1;
  int active_player_id = 0;          // 덱 도입 전까지 0 고정
  guint tick_source = 0;
};

App* g_app = nullptr;

// ---------- 피드백 송신 ----------

void SendFeedback(const std::string& type, const json& data) {
  json msg = {{"type", type}, {"data", data}};
  g_app->server.SendLine(msg.dump());
}

void SendInfo(const std::string& text) { SendFeedback("info", text); }
void SendWarn(const std::string& text) { SendFeedback("warn", text); }
void SendError(const std::string& text) { SendFeedback("error", text); }

// ---------- 플레이어 동작 (모두 GLib 메인루프 스레드에서 실행) ----------

std::optional<std::string> FilePathToUri(const std::string& utf8_path) {
  GError* err = nullptr;
  gchar* uri = gst_filename_to_uri(utf8_path.c_str(), &err);
  if (!uri) {
    if (err) g_error_free(err);
    return std::nullopt;
  }
  std::string result(uri);
  g_free(uri);
  return result;
}

void EmitMediaChanged() {
  json data = {{"idx", g_app->active_player_id}};
  if (g_app->current_file.contains("uuid")) data["uuid"] = g_app->current_file["uuid"];
  if (g_app->current_file.contains("path")) data["path"] = g_app->current_file["path"];
  if (g_app->playlist_track_index >= 0) {
    data["playlist_track_index"] = g_app->playlist_track_index;
  }
  SendFeedback("media_changed", data);
}

void PlayFile(const json& file) {
  const std::string path = file.value("path", "");
  if (path.empty()) {
    SendError("playid: file.path missing");
    return;
  }
  auto uri = FilePathToUri(path);
  if (!uri) {
    SendError("playid: invalid path: " + path);
    return;
  }

  g_app->current_file = file;
  gst_element_set_state(g_app->playbin, GST_STATE_NULL);
  g_object_set(g_app->playbin, "uri", uri->c_str(), nullptr);
  gst_element_set_state(g_app->playbin, GST_STATE_PLAYING);
  EmitMediaChanged();
}

void SetPlayState(GstState state) {
  if (g_app->playbin) gst_element_set_state(g_app->playbin, state);
}

void SeekMs(gint64 time_ms) {
  if (!g_app->playbin) return;
  gst_element_seek_simple(
      g_app->playbin, GST_FORMAT_TIME,
      static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
      time_ms * GST_MSECOND);
}

void ListAudioDevices() {
  GstDeviceMonitor* monitor = gst_device_monitor_new();
  GstCaps* caps = gst_caps_new_empty_simple("audio/x-raw");
  gst_device_monitor_add_filter(monitor, "Audio/Sink", caps);
  gst_caps_unref(caps);
  gst_device_monitor_start(monitor);

  json devices = json::array();
  GList* list = gst_device_monitor_get_devices(monitor);
  for (GList* it = list; it != nullptr; it = it->next) {
    GstDevice* dev = GST_DEVICE(it->data);
    gchar* name = gst_device_get_display_name(dev);
    GstStructure* props = gst_device_get_properties(dev);
    const gchar* dev_id =
        props ? gst_structure_get_string(props, "device.id") : nullptr;
    devices.push_back({{"deviceId", dev_id ? dev_id : (name ? name : "")},
                       {"name", name ? name : ""}});
    if (props) gst_structure_free(props);
    g_free(name);
  }
  g_list_free_full(list, gst_object_unref);
  gst_device_monitor_stop(monitor);
  gst_object_unref(monitor);

  SendFeedback("audiodevices", {{"devices", devices}});
}

// ---------- 100ms 상태 틱 ----------

gboolean OnTick(gpointer) {
  if (!g_app->playbin) return G_SOURCE_CONTINUE;

  GstState state = GST_STATE_NULL;
  gst_element_get_state(g_app->playbin, &state, nullptr, 0);
  if (state != GST_STATE_PLAYING && state != GST_STATE_PAUSED) return G_SOURCE_CONTINUE;

  gint64 pos_ns = -1, dur_ns = -1;
  gst_element_query_position(g_app->playbin, GST_FORMAT_TIME, &pos_ns);
  gst_element_query_duration(g_app->playbin, GST_FORMAT_TIME, &dur_ns);

  const gint64 time_ms = pos_ns >= 0 ? pos_ns / GST_MSECOND : 0;
  const gint64 dur_ms = dur_ns >= 0 ? dur_ns / GST_MSECOND : 0;
  const double position = dur_ms > 0 ? static_cast<double>(time_ms) / dur_ms : 0.0;

  SendFeedback("player_data",
               {{"id", g_app->active_player_id},
                {"event", state == GST_STATE_PLAYING ? "playing" : "paused"},
                {"time", time_ms},
                {"duration", dur_ms},
                {"position", position},
                {"is_playing", state == GST_STATE_PLAYING},
                {"state", state == GST_STATE_PLAYING ? "playing" : "paused"}});
  return G_SOURCE_CONTINUE;
}

// ---------- GStreamer 버스 ----------

gboolean OnBusMessage(GstBus*, GstMessage* msg, gpointer) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS: {
      // 트랙 진행/리핏 판단은 호스트(parser.js) 책임 — 여기서는 보고만 한다.
      SendFeedback("end_reached", {{"playlist_track_index", g_app->playlist_track_index},
                                   {"active_player_id", g_app->active_player_id}});
      break;
    }
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      SendError(std::string("pipeline error: ") + (err ? err->message : "unknown"));
      if (err) g_error_free(err);
      g_free(dbg);
      gst_element_set_state(g_app->playbin, GST_STATE_NULL);
      break;
    }
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

// ---------- 명령 디스패치 (GLib 메인루프 스레드) ----------

void HandleCommand(const json& msg) {
  const std::string cmd = msg.value("command", "");

  if (cmd == "playid" || cmd == "set_media") {
    if (msg.contains("file")) {
      if (msg.contains("track_idx")) g_app->playlist_track_index = msg["track_idx"];
      PlayFile(msg["file"]);
    } else {
      SendError(cmd + ": file missing");
    }
  } else if (cmd == "play") {
    SetPlayState(GST_STATE_PLAYING);
  } else if (cmd == "pause") {
    SetPlayState(GST_STATE_PAUSED);
  } else if (cmd == "stop" || cmd == "stop_all") {
    SetPlayState(GST_STATE_NULL);
    SendFeedback("player_data", {{"id", g_app->active_player_id},
                                 {"event", "stopped"},
                                 {"time", 0},
                                 {"duration", 0},
                                 {"position", 0.0},
                                 {"is_playing", false},
                                 {"state", "stopped"}});
  } else if (cmd == "set_time") {
    SeekMs(msg.value("time", static_cast<gint64>(0)));
  } else if (cmd == "get_audio_devices") {
    ListAudioDevices();
  } else if (cmd == "set_audio_device") {
    // TODO(deck): AudioOutput 팩토리 도입 시 wasapi2sink/asiosink device 적용
    SendWarn("set_audio_device: not implemented in spike");
  } else {
    // 미구현 명령은 디버그로 흘려보내되 프로토콜을 깨지 않는다
    SendFeedback("debug", "unhandled command: " + cmd);
  }
}

gboolean DispatchLineIdle(gpointer data) {
  auto* line = static_cast<std::string*>(data);
  try {
    HandleCommand(json::parse(*line));
  } catch (const std::exception& e) {
    SendError(std::string("bad command json: ") + e.what());
  }
  delete line;
  return G_SOURCE_REMOVE;
}

// ---------- 번들 GStreamer 격리 (배포 시) ----------

void ConfigureBundledGStreamer() {
  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  const auto bundle = std::filesystem::path(exe_path).parent_path() / L"gst-bundle";
  if (!std::filesystem::exists(bundle / L"plugins")) return;  // 개발 환경: 시스템 GStreamer 사용

  _wputenv_s(L"GST_PLUGIN_PATH", (bundle / L"plugins").c_str());
  _wputenv_s(L"GST_PLUGIN_SYSTEM_PATH_1_0", L"");
  wchar_t local_appdata[MAX_PATH];
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH) > 0) {
    const auto registry = std::filesystem::path(local_appdata) / L"vpapp" / L"gst-registry.bin";
    _wputenv_s(L"GST_REGISTRY_1_0", registry.c_str());
  }
  SetDllDirectoryW(bundle.c_str());
}

gboolean SendReady(gpointer) {
  SendInfo("Player ready");
  return G_SOURCE_REMOVE;
}

}  // namespace

int main(int argc, char* argv[]) {
  // pywin32의 우선순위 부스트 대체 (플랜: REALTIME 대신 안전한 ABOVE_NORMAL)
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  ConfigureBundledGStreamer();
  gst_init(&argc, &argv);

  App app;
  g_app = &app;
  app.loop = g_main_loop_new(nullptr, FALSE);

  app.playbin = gst_element_factory_make("playbin3", "player");
  if (!app.playbin) {
    fprintf(stderr, "FATAL: playbin3 unavailable (GStreamer plugins missing?)\n");
    return 1;
  }
  GstBus* bus = gst_element_get_bus(app.playbin);
  gst_bus_add_watch(bus, OnBusMessage, nullptr);
  gst_object_unref(bus);

  const uint16_t port = app.server.Start(1300, [](std::string line) {
    // 수신 스레드 → GLib 메인루프로 마샬링 (상태 변경은 단일 스레드에서만)
    g_main_context_invoke(nullptr, DispatchLineIdle, new std::string(std::move(line)));
  });
  if (port == 0) {
    fprintf(stderr, "FATAL: cannot bind control socket\n");
    return 1;
  }

  // stdout 계약: 포트 핸드셰이크 딱 한 줄, 단일 flush. 이후 stdout 사용 금지.
  printf("{\"type\": \"port\", \"data\": {\"port\": %u}}\n", port);
  fflush(stdout);

  app.tick_source = g_timeout_add(100, OnTick, nullptr);
  g_timeout_add(500, SendReady, nullptr);  // 호스트 접속 후 ready 통지 (Python과 동일 타이밍)

  g_main_loop_run(app.loop);

  app.server.Stop();
  gst_element_set_state(app.playbin, GST_STATE_NULL);
  gst_object_unref(app.playbin);
  g_main_loop_unref(app.loop);
  return 0;
}
