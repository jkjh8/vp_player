#pragma once

#include <windows.h>

#include <gst/gst.h>

#include <cstdint>
#include <functional>
#include <map>
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
//   [silence] audiotestsrc(live, mono) ─▶ audiomixer ─ capsfilter(버스 Nch)
//              ─▶ audioconvert!audioresample ─▶ wasapi2sink/asiosink
//
// 오디오 버스(Phase 3): 채널수 N = 출력 디바이스 추종 (wasapi min(ch,8) positioned /
// asio 드라이버 보고값 unpositioned). 각 브랜치 → 버스 채널 배치는 amix sink 패드의
// converter-config mix-matrix (file.channel_map, 없으면 항등 = v1 동작).
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
  // image_time_s: 이미지 표시 시간 (초, 0 = 무한) — is_image일 때만 의미
  // 반환값: 배정된 덱 슬롯 (0/1) — play_current_and_load_next에서 next가 같은 슬롯을
  // 고르지 않도록 avoid_slot으로 넘겨줌
  int PlayFile(const nlohmann::json& file, int track_idx, double image_time_s = 0.0);
  void PreloadNext(const nlohmann::json& file, int track_idx, double image_time_s = 0.0,
                    int avoid_slot = -1);
  bool Next();  // 프리롤된 대기 덱으로 스왑

  void Play();
  void Pause();
  void Stop();
  void SeekMs(int64_t time_ms);

  void SetBackgroundColor(uint32_t rgb);              // 0xRRGGBB
  nlohmann::json ListAudioDevices();                  // wasapi2 프로바이더만
  void SetAudioDevice(const std::string& device_id);  // 라이브 전환 (sink 교체)

  // 활성 덱(임베디드 오디오)의 라우팅/볼륨/뮤트를 재생 중 라이브 변경.
  // msg = {channel_map?, volume?, muted?}. 브랜치 채널 폭은 유지(로드 시 고정) —
  // channel_map은 현재 폭 내에서 amix 패드 matrix만 갱신한다.
  void SetDeckAudio(const nlohmann::json& msg);

  // 레거시 호환 경로 (프로토콜 §2: 현재 호스트 미사용이나 호환성 유지) + prev/next 폴백
  void SetPlaylistMode(bool on) { playlist_mode_ = on; }
  void SetTracks(const nlohmann::json& tracks);
  void SetTrackIndex(int idx) { track_index_ = idx; }
  bool PlayTrackIndex(int idx);  // 내부 tracks_ 기반 재생
  void Previous();               // track_index-1 (음수면 마지막으로 순환)

  // 로고 오버레이 (PNG/JPG = stb_image, SVG = lunasvg). 실제 표시 여부 =
  // 사용자 설정(show_logo) AND 미디어 규칙(이미지/비디오=숨김, 오디오/정지=표시)
  void SetLogoFile(const std::string& path);
  void SetLogoSize(int width_px);   // 0 = 원본 크기
  void SetLogoEnabled(bool show);   // show_logo 명령

  // 독립 오디오 트랙 (v2 §5) — 덱과 무관하게 병행 재생되는 오디오 전용 스트림.
  // 플레이리스트 병행 오디오 레인 / 타임라인 오디오 클립의 실행 단위.
  // msg = audio_track_play 명령 전체 ({track_id, file, volume?, channel_map?, loop?})
  void AudioTrackPlay(const nlohmann::json& msg);
  void AudioTrackStop(const std::string& track_id);
  void AudioTrackPause(const std::string& track_id);  // 덱 pause와 동일한 토글
  void AudioTrackSetVolume(const std::string& track_id, double volume);  // 0-100
  void AudioTrackSetChannelMap(const std::string& track_id, const nlohmann::json& map);
  void StopAllAudioTracks();

  void EmitTick();  // 100ms 타이머에서 호출 — player_data / audio_track_data 피드백

 private:
  struct Deck;
  struct AudioTrack;

  Deck* BuildDeck(int deck_id, const nlohmann::json& file, int track_idx, bool play_when_ready,
                  double image_time_s);
  void SwapTo(Deck* deck);
  void TeardownDeck(Deck* deck);
  bool CheckPreroll(Deck* deck);  // 50ms 폴링 콜백 본체
  GstClockTime RunningTime() const;

  bool CheckAudioTrackPreroll(AudioTrack* track);  // 50ms 폴링 콜백 본체
  void ConnectAudioTrack(AudioTrack* track);       // 프리롤 완료 → amix 연결
  void LoopAudioTrack(AudioTrack* track);          // SEGMENT_DONE/EOS → 0으로 재시크
  void TeardownAudioTrack(const std::string& track_id);
  void EmitAudioTrackData(AudioTrack* track, const char* state);

  bool InitLogoBranch();
  void PushLogoBuffer(int w, int h, const uint8_t* rgba);  // rgba 복사됨
  void ApplyLogoGeometry();
  void UpdateLogoVisibility(bool emit_feedback);
  // IDLE 프로브 콜백에서 실행 — sink 교체 + 버스 채널수/positioned 전환
  void DoAudioSinkSwap(const std::string& device_id, int channels, bool positioned);
  void ApplyDeckRouting(Deck* deck);  // channel_map → amix 패드 mix-matrix

  static void OnDecodePadAdded(GstElement* dbin, GstPad* pad, gpointer user_data);
  static void OnAudioTrackPadAdded(GstElement* dbin, GstPad* pad, gpointer user_data);
  static GstBusSyncReply OnBusSync(GstBus* bus, GstMessage* msg, gpointer user_data);
  static gboolean OnBusMessage(GstBus* bus, GstMessage* msg, gpointer user_data);

  FeedbackFn feedback_;
  HWND hwnd_ = nullptr;

  GstElement* pipeline_ = nullptr;
  GstElement* comp_ = nullptr;       // d3d11compositor (폴백: compositor)
  GstElement* amix_ = nullptr;       // audiomixer
  GstElement* bus_caps_ = nullptr;   // amix 직후 출력 capsfilter (버스 채널수 정책 지점)
  GstElement* audio_tail_ = nullptr; // 출력단 audioresample (sink 교체 시 재연결 지점)
  GstElement* audio_sink_ = nullptr; // wasapi2sink/asiosink (폴백: autoaudiosink)
  GstElement* bg_src_ = nullptr;     // videotestsrc solid-color
  GstPad* silence_pad_ = nullptr;    // 무음 앵커의 amix 요청 패드 (matrix 갱신 지점)
  int output_channels_ = 2;          // 오디오 버스 채널수 (디바이스 추종)
  bool bus_positioned_ = true;       // true = fallback mask, false = unpositioned(asio)
  bool use_d3d11_ = true;

  nlohmann::json tracks_ = nlohmann::json::array();  // set_tracks 사본 (레거시/폴백용)
  bool playlist_mode_ = false;
  int track_index_ = 0;

  std::unique_ptr<Deck> decks_[2];
  int live_deck_ = -1;     // 현재 화면/소리를 점유한 덱 id
  int standby_deck_ = -1;  // 프리롤 중/완료된 대기 덱 id
  bool paused_ = false;

  std::map<std::string, std::unique_ptr<AudioTrack>> audio_tracks_;  // track_id → 트랙

  // 로고 오버레이 상태
  GstElement* logo_src_ = nullptr;  // appsrc (RGBA) — 버퍼 1장 push, 컴포지터가 유지
  GstPad* logo_pad_ = nullptr;      // comp 요청 패드 (zorder 최상위)
  int logo_img_w_ = 0, logo_img_h_ = 0;
  int logo_size_px_ = 0;            // 표시 폭 (0 = 원본)
  bool logo_enabled_ = true;        // show_logo 사용자 설정
  bool media_wants_logo_ = true;    // 미디어 규칙 (정지 상태 = 표시)
  bool logo_loaded_ = false;
};

}  // namespace vp
