#pragma once

#include <windows.h>

#include <gst/gst.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace vp {

// 미디어 엔진 본체.
//
// 토폴로지 (플랜 "듀얼 덱 + 컴포지터" 확정안):
//   [bg] videotestsrc(solid-color, live) ─ d3d11upload ─▶ comp.sink (zorder 0)
//   [deck 0/1] uridecodebin3 ─ video: queue!d3d11upload!d3d11convert ─▶ comp.sink (zorder 1+id)
//                            └ audio: queue!audioconvert!audioresample!volume ─▶ amix.sink
//   d3d11compositor ─▶ d3d11videosink (자체 창 HWND, GstVideoOverlay)
//   [silence] audiotestsrc(live) ─▶ audiomixer ─▶ audioconvert!audioresample ─▶ wasapi2sink
//
// 덱 수명: Build(잠금+PAUSED, fakesink로 프리롤) → Swap(fakesink 제거, comp/amix
// 요청 패드 연결, 패드 오프셋 = 현재 러닝타임, 잠금 해제→PLAYING) → Teardown.
// 모든 공개 메서드는 GLib 메인루프 스레드에서만 호출한다.
class PlayerCore {
 public:
  using FeedbackFn = std::function<void(const std::string& type, const nlohmann::json& data)>;

  PlayerCore();   // Deck 완전 정의가 필요해 .cpp에서 정의 (unique_ptr pimpl 규칙)
  ~PlayerCore();

  bool Init(HWND video_hwnd, FeedbackFn feedback);
  void Shutdown();

  // file: 호스트가 주는 file 객체 (path 필수, uuid 등은 피드백에 반사)
  void PlayFile(const nlohmann::json& file, int track_idx);     // 프리롤 완료 즉시 스왑
  void PreloadNext(const nlohmann::json& file, int track_idx);  // 대기 덱 프리롤만
  bool Next();                                                  // 프리롤된 대기 덱으로 스왑

  void Play();
  void Pause();
  void Stop();
  void SeekMs(int64_t time_ms);

  void SetBackgroundColor(uint32_t rgb);              // 0xRRGGBB
  nlohmann::json ListAudioDevices();                  // wasapi2 프로바이더만
  void SetAudioDevice(const std::string& device_id);

  void EmitTick();  // 100ms 타이머에서 호출 — player_data 피드백

 private:
  struct Deck;

  Deck* BuildDeck(int deck_id, const nlohmann::json& file, int track_idx, bool play_when_ready);
  void SwapTo(Deck* deck);
  void TeardownDeck(Deck* deck);
  bool CheckPreroll(Deck* deck);  // 50ms 폴링 콜백 본체
  GstClockTime RunningTime() const;

  static void OnDecodePadAdded(GstElement* dbin, GstPad* pad, gpointer user_data);
  static GstBusSyncReply OnBusSync(GstBus* bus, GstMessage* msg, gpointer user_data);
  static gboolean OnBusMessage(GstBus* bus, GstMessage* msg, gpointer user_data);

  FeedbackFn feedback_;
  HWND hwnd_ = nullptr;

  GstElement* pipeline_ = nullptr;
  GstElement* comp_ = nullptr;       // d3d11compositor (폴백: compositor)
  GstElement* amix_ = nullptr;       // audiomixer
  GstElement* audio_sink_ = nullptr; // wasapi2sink (폴백: autoaudiosink)
  GstElement* bg_src_ = nullptr;     // videotestsrc solid-color
  bool use_d3d11_ = true;

  std::unique_ptr<Deck> decks_[2];
  int live_deck_ = -1;     // 현재 화면/소리를 점유한 덱 id
  int standby_deck_ = -1;  // 프리롤 중/완료된 대기 덱 id
  bool paused_ = false;
};

}  // namespace vp
