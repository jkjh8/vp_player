// 타임라인 엔진 (v2 §5.2 — Phase B). PlayerCore 의 타임라인 메서드 구현 (같은 클래스,
// 별도 번역 단위). 덱/오디오트랙 인프라를 재사용하며 decks_/BuildDeck/SwapTo/
// AudioTrackPlay 등 private 멤버에 직접 접근한다.
//
// 모델: NLE식 클립 배치(갭 허용). 비디오는 어느 시점이든 order(0=최상위)가 가장 위인
// 트랙의 클립 하나만 출력 → 타임라인을 "가시 세그먼트 시퀀스"(tl_segments_)로 컴파일,
// 각 에지 2초 전 standby 덱에 인포인트 프리롤 → 에지에서 SwapTo. 오디오 클립은 독립
// 오디오 트랙(§5.1)으로 에지에서 기동/정지. 클록 = 파이프라인 러닝타임 대비 epoch 오프셋.

#include "player/player_core.h"
#include "player/player_internal.h"

#include <algorithm>
#include <climits>

namespace vp {

using json = nlohmann::json;

namespace {
constexpr int64_t kTimelinePreroll = 2000;   // 세그먼트 진입 전 프리롤 리드타임(ms)
constexpr int64_t kPositionEmitMs = 250;     // timeline_position 스로틀
}  // namespace

// ---------------------------------------------------------------------------
// 컴파일: set_timeline JSON → tl_video_clips_ / tl_audio_clips_ / tl_segments_
// ---------------------------------------------------------------------------
void PlayerCore::TimelineCompile() {
  tl_video_clips_.clear();
  tl_audio_clips_.clear();
  tl_segments_.clear();
  tl_audio_on_.clear();

  const json& tracks = timeline_.contains("tracks") && timeline_["tracks"].is_array()
                           ? timeline_["tracks"]
                           : json::array();
  int64_t max_end = 0;

  for (const auto& tr : tracks) {
    const std::string type = tr.value("type", "video");
    const int order = tr.value("order", 0);
    const bool track_mute = tr.value("mute", false);
    const double track_vol = std::clamp(tr.value("volume", 100.0), 0.0, 100.0) / 100.0;
    const json track_map = tr.contains("channel_map") ? tr["channel_map"] : json();
    const json& clips = tr.contains("clips") && tr["clips"].is_array() ? tr["clips"] : json::array();

    for (const auto& cl : clips) {
      if (!cl.contains("file") || !cl["file"].is_object()) continue;
      const int64_t start = cl.value("start_ms", (int64_t)0);
      const int64_t in_ms = cl.value("in_ms", (int64_t)0);
      const int64_t out_ms = cl.value("out_ms", (int64_t)0);
      int64_t len = out_ms - in_ms;
      if (len <= 0) continue;
      const int64_t end = start + len;
      max_end = std::max(max_end, end);

      TlClip tc;
      tc.clip_id = cl.value("clip_id", std::string());
      tc.track_id = tr.value("track_id", std::string());
      tc.file = cl["file"];
      tc.start_ms = start;
      tc.end_ms = end;
      tc.in_ms = in_ms;
      tc.order = order;
      const double clip_vol = std::clamp(cl.value("volume", 100.0), 0.0, 100.0) / 100.0;

      if (type == "audio") {
        tc.volume = track_mute ? 0.0 : track_vol * clip_vol;
        tc.channel_map = track_map;  // 오디오 트랙 레벨 라우팅
        tl_audio_clips_.push_back(std::move(tc));
      } else {
        const std::string mt = tc.file.value("mimetype", std::string());
        tc.is_image = tc.file.value("is_image", false) || mt.rfind("image/", 0) == 0;
        tl_video_clips_.push_back(std::move(tc));
      }
    }
  }

  timeline_duration_ms_ = timeline_.value("duration_ms", (int64_t)0);
  if (timeline_duration_ms_ <= 0) timeline_duration_ms_ = max_end;
  tl_audio_on_.assign(tl_audio_clips_.size(), false);

  // 가시 세그먼트 컴파일: 모든 비디오 클립 에지에서 구간을 쪼갠 뒤 각 구간의 topmost
  // (min order) 클립을 고른다. 에지가 모든 경계를 포함하므로 각 클립은 구간을 완전히
  // 덮거나(포함) 전혀 겹치지 않는다.
  std::vector<int64_t> edges;
  edges.push_back(0);
  edges.push_back(timeline_duration_ms_);
  for (const auto& c : tl_video_clips_) {
    edges.push_back(c.start_ms);
    edges.push_back(c.end_ms);
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

  for (size_t i = 0; i + 1 < edges.size(); ++i) {
    const int64_t a = edges[i], b = edges[i + 1];
    if (a >= b) continue;
    int best = -1, best_order = INT_MAX;
    for (size_t ci = 0; ci < tl_video_clips_.size(); ++ci) {
      const TlClip& c = tl_video_clips_[ci];
      if (c.start_ms <= a && c.end_ms >= b && c.order < best_order) {
        best_order = c.order;
        best = static_cast<int>(ci);
      }
    }
    // 인접 동일 클립 병합
    if (!tl_segments_.empty() && tl_segments_.back().clip_index == best &&
        tl_segments_.back().end_ms == a) {
      tl_segments_.back().end_ms = b;
    } else {
      tl_segments_.push_back(TlSeg{a, b, best});
    }
  }

  feedback_("debug", json{{"timeline_compiled", timeline_id_},
                          {"video_clips", tl_video_clips_.size()},
                          {"audio_clips", tl_audio_clips_.size()},
                          {"segments", tl_segments_.size()},
                          {"duration_ms", timeline_duration_ms_}});
}

// ---------------------------------------------------------------------------
// 위치 / 세그먼트 조회
// ---------------------------------------------------------------------------
int64_t PlayerCore::TimelinePosMs() const {
  if (!timeline_playing_) return timeline_pause_pos_ms_;
  const int64_t pos =
      (static_cast<int64_t>(RunningTime()) - timeline_epoch_rt_) / GST_MSECOND;
  if (pos < 0) return 0;
  if (timeline_duration_ms_ > 0 && pos > timeline_duration_ms_) return timeline_duration_ms_;
  return pos;
}

int PlayerCore::TimelineSegAt(int64_t pos_ms) const {
  for (size_t i = 0; i < tl_segments_.size(); ++i) {
    const TlSeg& s = tl_segments_[i];
    if (pos_ms >= s.start_ms && pos_ms < s.end_ms) {
      return s.clip_index < 0 ? -1 : static_cast<int>(i);
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// 세그먼트 덱 빌드 (인포인트 시크 프리롤)
// ---------------------------------------------------------------------------
void PlayerCore::TimelineBuildSeg(int seg_idx, int64_t enter_pos_ms, bool play_when_ready) {
  if (seg_idx < 0 || seg_idx >= static_cast<int>(tl_segments_.size())) return;
  const TlSeg& seg = tl_segments_[seg_idx];
  if (seg.clip_index < 0) return;
  const TlClip& clip = tl_video_clips_[seg.clip_index];

  const int64_t enter = std::max(enter_pos_ms, seg.start_ms);
  const int64_t src_in = clip.in_ms + (enter - clip.start_ms);

  json f = clip.file;
  if (!clip.is_image) f["in_ms"] = src_in;  // 이미지는 프레임 하나뿐 → 인포인트 무의미

  Surface* s = DefaultSurface();
  if (!s) return;
  const int slot = (s->live_deck == 0) ? 1 : 0;  // 라이브와 다른 슬롯
  // 이미지 표시시간은 스케줄러가 세그먼트 경계에서 해체하므로 무한(0)으로 둔다.
  BuildDeck(s, slot, f, /*track_idx=*/-1, play_when_ready, /*image_time_s=*/0.0);
}

// ---------------------------------------------------------------------------
// 스케줄러 (100ms, EmitTick에서 호출)
// ---------------------------------------------------------------------------
void PlayerCore::TimelineScheduleVideo(int64_t pos_ms) {
  Surface* surf = DefaultSurface();
  if (!surf) return;
  const int want = TimelineSegAt(pos_ms);

  if (want != tl_live_seg_) {
    if (want < 0) {
      // 갭: 라이브 덱 숨김 → compositor 배경색
      if (surf->live_deck >= 0 && surf->decks[surf->live_deck])
        TeardownDeck(surf->decks[surf->live_deck].get());
      tl_live_seg_ = -1;
    } else if (tl_standby_seg_ == want && DeckPrerolled(surf, surf->standby_deck)) {
      SwapTo(surf->decks[surf->standby_deck].get());  // 프리롤된 standby → 라이브
      tl_live_seg_ = want;
      tl_standby_seg_ = -1;
    } else {
      // 프리롤 미스: 즉시 빌드(자동 스왑, 짧은 지연)
      TimelineBuildSeg(want, pos_ms, /*play_when_ready=*/true);
      tl_live_seg_ = want;  // 중복 빌드 방지 위해 eager 설정
      tl_standby_seg_ = -1;
    }
  }

  // 프리롤 룩어헤드: 2초 내 시작하는 다음 세그먼트 하나
  if (timeline_playing_) {
    for (size_t si = 0; si < tl_segments_.size(); ++si) {
      const TlSeg& seg = tl_segments_[si];
      if (seg.clip_index < 0) continue;
      if (static_cast<int>(si) == tl_live_seg_) continue;
      if (seg.start_ms > pos_ms && seg.start_ms <= pos_ms + kTimelinePreroll) {
        if (tl_standby_seg_ != static_cast<int>(si) && surf->standby_deck < 0) {
          TimelineBuildSeg(static_cast<int>(si), seg.start_ms, /*play_when_ready=*/false);
          tl_standby_seg_ = static_cast<int>(si);
        }
        break;  // 가장 임박한 하나만
      }
    }
  }
}

void PlayerCore::TimelineScheduleAudio(int64_t pos_ms) {
  if (!timeline_playing_) return;  // 일시정지/스크럽 중엔 오디오 기동 안 함
  for (size_t i = 0; i < tl_audio_clips_.size(); ++i) {
    const TlClip& c = tl_audio_clips_[i];
    const bool active = pos_ms >= c.start_ms && pos_ms < c.end_ms;
    const std::string id = "tlaud-" + c.clip_id;
    if (active && !tl_audio_on_[i]) {
      const int64_t src_in = c.in_ms + (pos_ms - c.start_ms);
      json msg;
      msg["track_id"] = id;
      msg["file"] = c.file;
      msg["in_ms"] = src_in;
      msg["volume"] = c.volume * 100.0;
      if (!c.channel_map.is_null() && c.channel_map.is_array())
        msg["channel_map"] = c.channel_map;
      AudioTrackPlay(msg);
      tl_audio_on_[i] = true;
    } else if (!active && tl_audio_on_[i]) {
      AudioTrackStop(id);
      tl_audio_on_[i] = false;
    }
  }
}

void PlayerCore::TimelineTick() {
  if (!timeline_active_) return;
  const int64_t pos = TimelinePosMs();

  // 종료 도달
  if (timeline_playing_ && timeline_duration_ms_ > 0 && pos >= timeline_duration_ms_) {
    timeline_playing_ = false;
    timeline_pause_pos_ms_ = timeline_duration_ms_;
    TimelineTeardown();
    if (Surface* s = DefaultSurface()) {
      s->media_wants_logo = true;
      UpdateLogoVisibility(s, /*emit_feedback=*/true);
    }
    feedback_("timeline_position", json{{"timeline_id", timeline_id_},
                                        {"time_ms", timeline_duration_ms_},
                                        {"duration_ms", timeline_duration_ms_},
                                        {"is_playing", false}});
    return;
  }

  TimelineScheduleVideo(pos);
  TimelineScheduleAudio(pos);

  // 위치 발신 (250ms 스로틀)
  const int64_t now_ms = g_get_monotonic_time() / 1000;
  if (now_ms - timeline_last_pos_emit_ >= kPositionEmitMs) {
    timeline_last_pos_emit_ = now_ms;
    feedback_("timeline_position", json{{"timeline_id", timeline_id_},
                                        {"time_ms", pos},
                                        {"duration_ms", timeline_duration_ms_},
                                        {"is_playing", timeline_playing_}});
  }
}

// ---------------------------------------------------------------------------
// 해체 (모드 유지)
// ---------------------------------------------------------------------------
void PlayerCore::TimelineTeardown() {
  if (Surface* s = DefaultSurface()) {
    if (s->live_deck >= 0 && s->decks[s->live_deck]) TeardownDeck(s->decks[s->live_deck].get());
    if (s->standby_deck >= 0 && s->decks[s->standby_deck])
      TeardownDeck(s->decks[s->standby_deck].get());
  }
  StopAllAudioTracks();
  tl_live_seg_ = -1;
  tl_standby_seg_ = -1;
  std::fill(tl_audio_on_.begin(), tl_audio_on_.end(), false);
}

// ---------------------------------------------------------------------------
// 명령 (main.cpp HandleCommand → 위임)
// ---------------------------------------------------------------------------
void PlayerCore::SetTimeline(const json& msg) {
  // 기존 재생(플레이리스트/타임라인) 정리 후 새 시트 로드
  TimelineTeardown();
  timeline_ = msg;
  timeline_id_ = msg.value("timeline_id", std::string());
  timeline_active_ = true;
  timeline_playing_ = false;
  timeline_pause_pos_ms_ = 0;
  timeline_epoch_rt_ = 0;
  TimelineCompile();
  feedback_("info", "timeline loaded: " + timeline_id_);
}

void PlayerCore::TimelinePlay(int64_t time_ms, bool have_time) {
  if (!timeline_active_) {
    feedback_("warn", "timeline_play: no timeline loaded");
    return;
  }
  int64_t start = have_time ? time_ms : timeline_pause_pos_ms_;
  start = std::clamp<int64_t>(start, 0, timeline_duration_ms_);

  TimelineTeardown();  // 깨끗한 시작 (덱/오디오 재구성)
  timeline_epoch_rt_ = static_cast<int64_t>(RunningTime()) - start * GST_MSECOND;
  timeline_pause_pos_ms_ = start;
  timeline_playing_ = true;
  timeline_last_pos_emit_ = 0;

  if (Surface* s = DefaultSurface()) {
    s->media_wants_logo = false;  // 재생 중 로고 숨김
    UpdateLogoVisibility(s, /*emit_feedback=*/true);
  }

  // 첫 세그먼트 즉시 스케줄 (틱 대기 지연 감소)
  const int64_t pos = TimelinePosMs();
  TimelineScheduleVideo(pos);
  TimelineScheduleAudio(pos);
}

void PlayerCore::TimelinePause() {
  if (!timeline_active_) return;
  if (timeline_playing_) {
    // 재생 → 일시정지: 위치 동결 + 덱/오디오 블록
    timeline_pause_pos_ms_ = TimelinePosMs();
    timeline_playing_ = false;
    Pause();  // 라이브 덱 일시정지
    for (size_t i = 0; i < tl_audio_clips_.size(); ++i) {
      if (tl_audio_on_[i]) AudioTrackPause("tlaud-" + tl_audio_clips_[i].clip_id);
    }
  } else {
    // 일시정지 → 재개
    timeline_epoch_rt_ =
        static_cast<int64_t>(RunningTime()) - timeline_pause_pos_ms_ * GST_MSECOND;
    timeline_playing_ = true;
    Play();
    for (size_t i = 0; i < tl_audio_clips_.size(); ++i) {
      if (tl_audio_on_[i]) AudioTrackPause("tlaud-" + tl_audio_clips_[i].clip_id);  // 토글 복귀
    }
  }
}

void PlayerCore::TimelineSeek(int64_t time_ms) {
  if (!timeline_active_) return;
  const int64_t t = std::clamp<int64_t>(time_ms, 0, timeline_duration_ms_);
  // 성능 캐비앗: 재생 중 덱 해체 + 신규 세그먼트의 in_ms ACCURATE 시크(4K long-GOP)는
  // 키프레임부터 타깃까지 디코드가 필요해 수백ms~수초 소요될 수 있고, 그 동안 메인 루프가
  // 정지해 위치 발신이 잠깐 멈춘다(시크 착지값은 정확). 후속 최적화 후보: 프리롤엔 키프레임
  // 스냅 시크(비ACCURATE) 사용 또는 시크를 별도 스레드로. 현재는 정확도 우선으로 ACCURATE 유지.
  TimelineTeardown();  // 전체 해체 후 신규 시각 기준 재구성
  if (timeline_playing_) {
    timeline_epoch_rt_ = static_cast<int64_t>(RunningTime()) - t * GST_MSECOND;
  }
  timeline_pause_pos_ms_ = t;
  // 현재 세그먼트를 즉시 반영 (일시정지 중 스크럽이면 프레임 표시)
  const int64_t pos = TimelinePosMs();
  TimelineScheduleVideo(pos);
  TimelineScheduleAudio(pos);
}

void PlayerCore::TimelineStop() {
  TimelineTeardown();
  timeline_active_ = false;
  timeline_playing_ = false;
  timeline_pause_pos_ms_ = 0;
  timeline_epoch_rt_ = 0;
  if (Surface* s = DefaultSurface()) {
    s->media_wants_logo = true;  // 정지 → 로고 복귀 (§2.7 규약과 정합)
    UpdateLogoVisibility(s, /*emit_feedback=*/true);
  }
  feedback_("timeline_position", json{{"timeline_id", timeline_id_},
                                      {"time_ms", 0},
                                      {"duration_ms", timeline_duration_ms_},
                                      {"is_playing", false}});
}

}  // namespace vp
