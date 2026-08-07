#pragma once

// PlayerCore 내부 구조체 정의 (Deck / AudioTrack / Surface). 두 번역 단위
// (player_core.cpp, player_timeline.cpp)가 공유하므로 헤더로 분리한다.
// 외부에는 노출하지 않는다 (player_core.h만 공개 API).

#include <windows.h>

#include <gst/gst.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "player/player_core.h"
#include "video/video_window.h"

namespace vp {

// 채널별 라우팅: 소스 채널 s → 버스 채널 out, gain(0~1), muted.
struct ChannelRoute {
  int out = -1;
  float gain = 1.0f;
  bool muted = false;
};

// ---------------------------------------------------------------------------
// Deck — A/B 갭리스 재생 슬롯. 비디오는 소속 Surface의 comp로, 오디오는 전역 amix로.
// ---------------------------------------------------------------------------
struct PlayerCore::Deck {
  PlayerCore* core = nullptr;
  Surface* surface = nullptr;
  int id = 0;
  nlohmann::json file;
  int track_idx = -1;
  bool play_when_ready = false;

  GstElement* bin = nullptr;
  GstElement* decode = nullptr;  // uridecodebin3

  GstElement* video_tail = nullptr;
  GstPad* video_out = nullptr;
  GstPad* video_ghost = nullptr;
  gulong video_block = 0;
  std::atomic<bool> video_ready{false};
  GstElement* audio_tail = nullptr;
  GstPad* audio_out = nullptr;
  GstPad* audio_ghost = nullptr;
  gulong audio_block = 0;
  std::atomic<bool> audio_ready{false};

  GstPad* comp_pad = nullptr;
  GstPad* amix_pad = nullptr;

  enum class State { Building, Prerolled, Live, Dead };
  State state = State::Building;
  bool eos_sent = false;
  int tl_seg = -1;
  guint preroll_watch = 0;
  GstClockTime preroll_started = 0;
  GstClockTime paused_running = GST_CLOCK_TIME_NONE;

  std::vector<ChannelRoute> channel_routes;
  int branch_channels = 2;
  double volume_gain = 1.0;
  bool master_muted = false;

  gint64 in_ms = 0;
  bool inpoint_seeked = false;

  // 트랙별 시작 지연: 프리롤 완료 후 실제 표시까지 지연 (ms, 0=즉시)
  gint64 delay_ms = 0;
  guint delay_timer = 0;

  // 멀티 PC 동기(Phase 5): 공유 러닝타임 기준 절대 시작 시각(ns, -1=미지정).
  // 지정 시 SwapTo가 pad offset을 이 값으로 설정 → PTP로 러닝타임이 동기화된 전 PC가
  // 같은 시각에 첫 프레임을 표시(락스텝). file.start_at.
  gint64 start_at_rt = -1;

  // 로컬 멀티윈도우 동기(play_synced 배리어): 이 덱이 대기 중인 동기 그룹의 멤버.
  // true면 프리롤 완료 시 개별 스왑을 하지 않고 배리어(FireSyncGroup)가 일괄 스왑을 소유.
  bool sync_pending = false;

  bool is_image = false;
  gint64 image_time_ms = 0;
  guint image_timer = 0;
  GstClockTime image_started = 0;
  gint64 image_elapsed_ms = 0;

  // 프리롤 실패 원인 힌트 (버스 핸들러가 DeckForObject로 이 덱에 매핑해 설정). CheckPreroll이
  // 실패 시 이 힌트로 playback_error의 reason을 정한다.
  //  codec_error    : missing-plugin / 스트림 코덱 에러 (HW 전용에서 GPU가 못 여는 코덱 포함)
  //  resource_error : filesrc 파일 열기 실패 (경로 없음/잠김 등)
  bool codec_error = false;
  bool resource_error = false;
};

// ---------------------------------------------------------------------------
// AudioTrack — 독립 오디오 트랙 (v2 §5). 전역 (창 개념 없음, 공유 amix).
// ---------------------------------------------------------------------------
struct PlayerCore::AudioTrack {
  PlayerCore* core = nullptr;
  std::string id;
  nlohmann::json file;

  GstElement* bin = nullptr;
  GstElement* decode = nullptr;
  GstElement* volume_el = nullptr;
  GstPad* out = nullptr;
  GstPad* ghost = nullptr;
  gulong block = 0;
  gulong eos_probe = 0;
  std::atomic<bool> ready{false};
  GstPad* amix_pad = nullptr;

  enum class State { Building, Live, Dead };
  State state = State::Building;
  bool paused = false;
  guint preroll_watch = 0;
  GstClockTime preroll_started = 0;
  GstClockTime paused_running = GST_CLOCK_TIME_NONE;

  bool loop = false;
  bool stopped_sent = false;
  gint64 in_ms = 0;
  bool inpoint_seeked = false;
  std::vector<ChannelRoute> channel_routes;
  int branch_channels = 2;
  double volume_gain = 1.0;
  bool master_muted = false;

  // 오디오 트랙별 시작 지연(ms): 프리롤 완료 후 amix 연결(ConnectAudioTrack)까지 대기.
  gint64 delay_ms = 0;
  guint delay_timer = 0;
};

// ---------------------------------------------------------------------------
// Surface — 창 1개 단위 (comp/vsink/듀얼덱/배경/로고). 오디오는 전역 버스 공유.
// ---------------------------------------------------------------------------
struct PlayerCore::Surface {
  PlayerCore* core = nullptr;
  int id = 0;
  VideoWindow window;
  HWND hwnd = nullptr;

  GstElement* comp = nullptr;    // d3d11compositor (폴백: compositor)
  GstElement* vsink = nullptr;   // d3d11videosink (폴백: autovideosink)
  GstElement* bg_src = nullptr;  // videotestsrc solid-color

  std::string aspect_mode = "letterbox";
  int aspect_target_w = 0;
  int aspect_target_h = 0;
  bool overlay_ready = false;

  std::unique_ptr<Deck> decks[2];
  int live_deck = -1;
  int standby_deck = -1;
  bool paused = false;

  // 전 트랙 프리롤 풀 (요구사항 #4). sequence = 이 창의 전체 트랙 목록(preload_playlist),
  // pool = 미리 디코더에 프리롤해 둔 대기 덱들(A/B 라이브 슬롯과 별개, id=-1로 파킹).
  // 재생 명령 시 pool에서 경로가 일치하는 덱을 즉시 승격(swap)해 지연 없이 전환한다.
  std::vector<nlohmann::json> sequence;
  std::vector<std::unique_ptr<Deck>> pool;

  // 로고 오버레이 상태 (창별)
  GstElement* logo_src = nullptr;
  GstPad* logo_pad = nullptr;
  int logo_img_w = 0, logo_img_h = 0;
  int logo_size_px = 0;
  bool logo_enabled = true;
  bool media_wants_logo = true;
  bool logo_loaded = false;
};

// ---------------------------------------------------------------------------
// SyncGroup — play_synced 배리어. 여러 창의 덱을 모아 전원이 Prerolled 될 때까지
// 기다렸다가, 공유 러닝타임 기준 start_at을 한 번 계산해 동시에 스왑한다(락스텝).
// 동시에 하나만 대기(새 play_synced가 오면 기존 그룹을 강제 발화).
// ---------------------------------------------------------------------------
struct PlayerCore::SyncGroup {
  int scene_idx = -1;
  gint64 lead_ns = 120 * GST_MSECOND;   // 프리롤 완료 후 스왑/링크 여유
  gint64 explicit_start_at = -1;        // >=0 = 외부(PTP master) 지정값 사용, 로컬 계산 생략
  guint timeout_id = 0;                 // 타임아웃 폴백 소스
  std::vector<Deck*> members;           // 함께 스왑할 덱들 (소유는 Surface A/B 슬롯)
};

}  // namespace vp
