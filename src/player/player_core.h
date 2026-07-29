#pragma once

#include <windows.h>

#include <gst/gst.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "video/video_window.h"

namespace vp {

// 미디어 엔진 본체.
//
// 토폴로지 (플랜 "듀얼 덱 + 컴포지터" 확정안 — Phase 1에서 멀티 서피스로 확장):
//   [공유 파이프라인, 시스템/PTP 클록 1개]
//   창(Surface)별:
//     [bg] videotestsrc(solid-color, live) ─ d3d11upload ─▶ comp.sink (zorder 0)
//     [deck 0/1] uridecodebin3 ─ video: queue!d3d11upload!d3d11convert ─▶ comp.sink (zorder 1+id)
//     [logo] appsrc(RGBA) ─▶ comp.sink (zorder 100)
//     d3d11compositor ─▶ d3d11videosink (창 HWND, GstVideoOverlay)
//   전역 오디오 버스 (모든 창의 모든 덱이 공유):
//     [decks audio] ─ queue!audioconvert!audioresample!volume ─▶ amix.sink
//     [silence] audiotestsrc(live, mono) ─▶ amix ─ capsfilter(버스 Nch)
//                ─▶ audioconvert!audioresample ─▶ wasapi2sink/asiosink
//
// 멀티 윈도우(Phase 1): 창마다 독립 comp/vsink/듀얼덱/배경/로고 = struct Surface. 오디오
// 버스는 전역 공유(전 창 덱이 amix로 믹스). 재생/피드백은 window_id로 주소지정. 파이프라인
// 클록은 전역 1개 — 창 간(그리고 멀티 PC PTP) 동기의 기준.
//
// 오디오 버스(Phase 3): 채널수 N = 출력 디바이스 추종. 각 브랜치 → 버스 채널 배치는 amix
// sink 패드의 converter-config mix-matrix (file.channel_map, 없으면 항등 = v1 동작).
//
// 덱 수명: Build(잠금+PAUSED, tail 블록 프로브로 프리롤) → Swap(고스트 패드로 comp/amix
// 연결, 패드 오프셋 = start_at 또는 현재 러닝타임, 잠금 해제) → Teardown.
// 모든 공개 메서드는 GLib 메인루프 스레드에서만 호출한다.
class PlayerCore {
 public:
  using FeedbackFn = std::function<void(const std::string& type, const nlohmann::json& data)>;
  // 특정 창이 사용자에 의해 닫힐 때 호출 (window_id 전달). 주 창(0) 닫힘 = 앱 종료 판단은
  // 호출자(main.cpp)의 몫.
  using SurfaceClosedFn = std::function<void(int window_id)>;

  PlayerCore();   // Deck/Surface 완전 정의가 필요해 .cpp에서 정의 (unique_ptr pimpl 규칙)
  ~PlayerCore();

  // 공유 파이프라인 + 전역 오디오 버스만 구성 (창은 CreateSurface로 별도 생성)
  bool Init(FeedbackFn feedback);
  void Shutdown();

  void SetSurfaceClosedHandler(SurfaceClosedFn fn) { on_surface_closed_ = std::move(fn); }

  // ---- 창(Surface) 생명주기 -------------------------------------------------
  // 창 생성: 자체 Win32 창 + comp/vsink/배경/로고 브랜치를 공유 파이프라인에 추가.
  bool CreateSurface(int window_id, const WindowPlacement& placement,
                     const std::string& aspect_mode = "letterbox");
  void DestroySurface(int window_id);
  bool HasSurface(int window_id) const { return surfaces_.count(window_id) > 0; }
  // 기본 대상 창 id — 주 창 개념 폐지 후 window_id 미지정 명령의 라우팅 대상.
  // 창 0이 있으면 0, 없으면 존재하는 첫 창, 아무 창도 없으면 0.
  int DefaultWindowId() const {
    if (surfaces_.count(0)) return 0;
    if (!surfaces_.empty()) return surfaces_.begin()->first;
    return 0;
  }
  nlohmann::json ListSurfaces() const;  // get_windows 피드백용
  // 엔진 통계 (memory_status 피드백용): 창/라이브덱/프리롤덱/오디오트랙 수
  nlohmann::json EngineStats() const;
  // 창 재배치 (라이브 모니터/좌표/크기) — 창 스레드로 마샬링
  void ApplyDisplayPlacement(const WindowPlacement& placement, int window_id = 0);
  void SetFullscreen(bool fullscreen, int window_id = 0);

  // ---- 재생 (window_id 기본 0 = 주 창, 하위호환) -----------------------------
  // file: 호스트가 주는 file 객체 (path 필수). image_time_s: 이미지 표시 시간(초, 0=무한).
  // delay_ms: 프리롤 완료 후 실제 표시까지 지연(ms, 0=즉시) — file.delay_ms.
  // 반환값: 배정된 덱 슬롯 (0/1) — play_current_and_load_next에서 next가 같은 슬롯을
  // 고르지 않도록 avoid_slot으로 넘겨줌.
  int PlayFile(const nlohmann::json& file, int track_idx, double image_time_s = 0.0,
               int window_id = 0);
  void PreloadNext(const nlohmann::json& file, int track_idx, double image_time_s = 0.0,
                   int avoid_slot = -1, int window_id = 0);
  bool Next(int window_id = 0);  // 프리롤된 대기 덱으로 스왑

  // 전 트랙 프리롤 (요구사항 #4). set_preload_config: 창당 룩어헤드 + 전역 덱 상한.
  // preload_playlist: 창의 전체 시퀀스를 받아 상한 내에서 앞선 트랙들을 풀에 프리롤.
  void SetPreloadConfig(const nlohmann::json& msg);
  void PreloadPlaylist(const nlohmann::json& msg);

  void Play(int window_id = 0);
  void Pause(int window_id = 0);
  void Stop(int window_id = 0);
  void StopAll();  // 전 창 정지 + 오디오 트랙 전부 정지 (stop_all)
  void SeekMs(int64_t time_ms, int window_id = 0);

  // 현재 화면/소리를 점유한 덱 id (0/1), 없으면 -1. Stop() 전 player_data 피드백에 실을
  // id 캡처용 (host pStatus와 id 불일치 시 상태 갱신이 무시됨).
  int ActiveDeckId(int window_id = 0) const;

  void SetBackgroundColor(uint32_t rgb, int window_id = 0);  // 0xRRGGBB
  // 캔버스(1920x1080)를 창 크기에 맞추는 방식. mode: letterbox|crop|stretch.
  void SetAspectMode(const std::string& mode, int target_w, int target_h, int window_id = 0);
  void SetDisplaySize(int target_w, int target_h, int window_id = 0);

  nlohmann::json ListAudioDevices();                  // wasapi2/asio 프로바이더
  void SetAudioDevice(const std::string& device_id);  // 라이브 전환 (sink 교체) — 전역
  // 활성 덱(임베디드 오디오)의 라우팅/볼륨/뮤트 라이브 변경. msg={channel_map?,volume?,muted?}.
  void SetDeckAudio(const nlohmann::json& msg, int window_id = 0);

  // 레거시 호환 경로 (프로토콜 §2) + prev/next 폴백 — 주 창(0) 기준
  void SetPlaylistMode(bool on) { playlist_mode_ = on; }
  void SetTracks(const nlohmann::json& tracks);
  void SetTrackIndex(int idx) { track_index_ = idx; }
  bool PlayTrackIndex(int idx);
  void Previous();

  // 로고 오버레이 (PNG/JPG=stb_image, SVG=lunasvg). 창별. 실제 표시 = show_logo AND 미디어 규칙.
  void SetLogoFile(const std::string& path, int window_id = 0);
  void SetLogoSize(int width_px, int window_id = 0);   // 0 = 원본
  void SetLogoEnabled(bool show, int window_id = 0);

  // 독립 오디오 트랙 (v2 §5) — 덱과 무관, 전역 (창 개념 없음, 공유 amix).
  void AudioTrackPlay(const nlohmann::json& msg);
  void AudioTrackStop(const std::string& track_id);
  void AudioTrackPause(const std::string& track_id);
  void AudioTrackSetVolume(const std::string& track_id, double volume);
  void AudioTrackSetChannelMap(const std::string& track_id, const nlohmann::json& map);
  void StopAllAudioTracks();

  // 타임라인 모드 (v2 §5.2 — Phase B). 주 창(0)에서 단일 비디오 출력.
  void SetTimeline(const nlohmann::json& msg);
  void TimelinePlay(int64_t time_ms, bool have_time);
  void TimelinePause();
  void TimelineSeek(int64_t time_ms);
  void TimelineStop();
  bool TimelineActive() const { return timeline_active_; }

  void EmitTick();  // 100ms 타이머 — 전 창 player_data / audio_track_data / 타임라인 스케줄

  // ---- 멀티 PC 클럭 동기 (Phase 5) ----
  // PTP(IEEE 1588) 멀티캐스트 클럭으로 전환 (전 PC가 같은 절대 시각 공유). 기본은 시스템
  // 클록이며 명시 호출 시에만 전환 — 단일 머신 동작은 불변.
  void EnablePtp(int domain);
  void SetPtpBaseTime(int64_t base_time);  // slave: master base_time 맞춰 러닝타임 동기
  int64_t GetRunningTimeNs() const;        // 현재 공유 러닝타임(ns) — start_at 계산 기준
  nlohmann::json PtpStatus() const;

 private:
  struct Deck;
  struct AudioTrack;
  struct Surface;  // 창 1개 단위 (comp/vsink/듀얼덱/배경/로고). 정의는 .cpp.

  // 타임라인 컴파일 결과 (플레인 데이터).
  struct TlClip {
    std::string clip_id;
    std::string track_id;
    nlohmann::json file;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int64_t in_ms = 0;
    int order = 0;
    double volume = 1.0;
    nlohmann::json channel_map;
    bool is_image = false;
  };
  struct TlSeg {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int clip_index = -1;
  };

  // ---- 서피스 조회/구성 헬퍼 ----
  Surface* GetSurface(int window_id);
  const Surface* GetSurface(int window_id) const;
  Surface* DefaultSurface() { return GetSurface(DefaultWindowId()); }  // 타임라인/레거시 기준 창
  bool BuildSurfaceGraph(Surface* s);   // comp/vsink/bg/logo 브랜치 생성·링크
  void TeardownSurfaceGraph(Surface* s);
  bool InitLogoBranch(Surface* s);
  void PushLogoBuffer(Surface* s, int w, int h, const uint8_t* rgba);
  void ApplyLogoGeometry(Surface* s);
  void UpdateLogoVisibility(Surface* s, bool emit_feedback);
  void ApplyVideoGeometry(Surface* s);
  Surface* SurfaceForVsink(GstElement* vsink);  // OnBusSync 오버레이 바인딩 매칭

  // ---- 덱 ----
  // 덱 bin 구성 + 프리롤 워치 시작 (저장은 호출자). id<0 = 풀 파킹 덱.
  std::unique_ptr<Deck> ConstructDeck(Surface* s, int id, const nlohmann::json& file, int track_idx,
                                      bool play_when_ready, double image_time_s, int64_t delay_ms);
  Deck* BuildDeck(Surface* s, int deck_id, const nlohmann::json& file, int track_idx,
                  bool play_when_ready, double image_time_s, int64_t delay_ms = 0);
  void SwapTo(Deck* deck);
  void SwapWithDelay(Deck* deck);  // delay_ms 반영 스왑 (프리롤 완료 후)
  void TeardownDeck(Deck* deck);
  bool CheckPreroll(Deck* deck);
  bool PoolPrerollIndex(Surface* s, int seq_idx);  // 시퀀스 인덱스 프리롤 (커버/상한 판정)
  GstClockTime RunningTime() const;

  // ---- 프리롤 풀 ----
  int PoolFindByPath(Surface* s, const std::string& path);  // pool 인덱스 (-1 없음)
  // 풀 덱을 A/B 슬롯으로 승격. swap_now=true면 즉시(또는 delay 후) 스왑, false면 standby로.
  void PromotePooled(Surface* s, int pool_idx, bool swap_now);
  void FillPool(Surface* s, int current_idx);   // [current+1 .. current+lookahead] 프리롤
  int CountPrerollDecks() const;                 // 전역 프리롤/빌딩 덱 수 (상한 판정)
  void ClearPool(Surface* s);

  // ---- 오디오 트랙 ----
  bool CheckAudioTrackPreroll(AudioTrack* track);
  void ConnectAudioTrack(AudioTrack* track);
  void LoopAudioTrack(AudioTrack* track);
  void TeardownAudioTrack(const std::string& track_id);
  void EmitAudioTrackData(AudioTrack* track, const char* state);

  // ---- 타임라인 (player_timeline.cpp) — 주 창(0) 기준 ----
  void TimelineCompile();
  void TimelineTeardown();
  void TimelineTick();
  void TimelineScheduleVideo(int64_t pos_ms);
  void TimelineScheduleAudio(int64_t pos_ms);
  void TimelineBuildSeg(int seg_idx, int64_t enter_pos_ms, bool play_when_ready);
  int  TimelineSegAt(int64_t pos_ms) const;
  int64_t TimelinePosMs() const;
  bool DeckPrerolled(Surface* s, int slot) const;

  // 오디오 버스 라우팅
  void DoAudioSinkSwap(const std::string& device_id, int channels, bool positioned);
  void ApplyDeckRouting(Deck* deck);

  static void OnDecodePadAdded(GstElement* dbin, GstPad* pad, gpointer user_data);
  static void OnAudioTrackPadAdded(GstElement* dbin, GstPad* pad, gpointer user_data);
  static GstBusSyncReply OnBusSync(GstBus* bus, GstMessage* msg, gpointer user_data);
  static gboolean OnBusMessage(GstBus* bus, GstMessage* msg, gpointer user_data);

  FeedbackFn feedback_;
  SurfaceClosedFn on_surface_closed_;

  GstElement* pipeline_ = nullptr;
  // 전역 오디오 버스 (모든 창 공유)
  GstElement* amix_ = nullptr;       // audiomixer
  GstElement* bus_caps_ = nullptr;   // amix 직후 출력 capsfilter (버스 채널수 정책 지점)
  GstElement* audio_tail_ = nullptr; // 출력단 audioresample (sink 교체 시 재연결 지점)
  GstElement* audio_sink_ = nullptr; // wasapi2sink/asiosink (폴백: autoaudiosink)
  GstPad* silence_pad_ = nullptr;    // 무음 앵커의 amix 요청 패드 (matrix 갱신 지점)
  int output_channels_ = 2;          // 오디오 버스 채널수 (디바이스 추종)
  bool bus_positioned_ = true;       // true = fallback mask, false = unpositioned(asio)
  bool use_d3d11_ = true;

  // 멀티 PC PTP 동기 (Phase 5). 기본 비활성 = 시스템 클록.
  GstClock* ptp_clock_ = nullptr;
  bool ptp_enabled_ = false;
  int ptp_domain_ = 0;

  std::map<int, std::unique_ptr<Surface>> surfaces_;  // window_id → 창

  nlohmann::json tracks_ = nlohmann::json::array();  // set_tracks 사본 (레거시/폴백용)
  bool playlist_mode_ = false;
  int track_index_ = 0;

  // 프리롤 풀 설정. lookahead = 창당 미리 프리롤할 다음 트랙 수(1 = 기존 1-ahead 동작).
  // max_decks = 전역 동시 프리롤 덱 상한(초과 시 먼 트랙은 프리롤 생략 = 메타 강등).
  int preload_lookahead_ = 1;
  int preload_max_decks_ = 8;

  std::map<std::string, std::unique_ptr<AudioTrack>> audio_tracks_;  // track_id → 트랙 (전역)

  // 타임라인 모드 상태 (player_timeline.cpp) — 주 창(0)에서 동작
  bool timeline_active_ = false;
  bool timeline_playing_ = false;
  nlohmann::json timeline_;
  std::string timeline_id_;
  int64_t timeline_duration_ms_ = 0;
  int64_t timeline_epoch_rt_ = 0;
  int64_t timeline_pause_pos_ms_ = 0;
  int64_t timeline_last_pos_emit_ = 0;
  std::vector<TlClip> tl_video_clips_;
  std::vector<TlClip> tl_audio_clips_;
  std::vector<TlSeg> tl_segments_;
  std::vector<bool> tl_audio_on_;
  int tl_live_seg_ = -1;
  int tl_standby_seg_ = -1;
};

}  // namespace vp
