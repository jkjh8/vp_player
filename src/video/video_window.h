#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace vp {

// 창을 배치할 모니터/좌표/크기. width/height=0은 모니터 작업영역 전체 크기를 의미.
struct WindowPlacement {
  int monitor_index = -1;  // -1 = primary
  int x = 0, y = 0;        // 모니터 좌상단 기준 오프셋
  int width = 0, height = 0;
};

// 비디오 출력용 자체 Win32 창.
// 별도 스레드에서 메시지 펌프를 돌리며(플랜의 Thread A), 창 조작은 전부
// PostMessage로 창 스레드에 마샬링된다. HWND는 d3d11videosink의
// GstVideoOverlay에 넘겨진다.
class VideoWindow {
 public:
  // 사용자가 창을 닫을 때 호출 (창 스레드에서 호출됨 — 호출자가 마샬링할 것)
  using CloseHandler = std::function<void()>;
  // 로컬 F11 키 입력으로 풀스크린 상태가 바뀔 때 호출 (창 스레드에서 호출됨 —
  // 호출자가 마샬링할 것). 호스트가 명령으로 SetFullscreen()을 호출한 경우는
  // 호출자(main.cpp)가 이미 피드백을 보내므로 재호출되지 않는다.
  using FullscreenChangeHandler = std::function<void(bool)>;
  // 사용자가 창을 직접 드래그로 이동/리사이즈했을 때 호출 (창 스레드 — 호출자가 마샬링).
  // 호스트가 ApplyPlacement()로 배치한 경우는 프로그램적 변경이라 호출되지 않는다
  // (WM_EXITSIZEMOVE = 사용자 상호작용 종료 시점에만 발생).
  using PlacementChangeHandler = std::function<void(const WindowPlacement&)>;

  VideoWindow() = default;
  ~VideoWindow();

  VideoWindow(const VideoWindow&) = delete;
  VideoWindow& operator=(const VideoWindow&) = delete;

  // 창 생성 (블로킹 — 생성 완료 후 반환). 성공 시 true.
  bool Create(const WindowPlacement& placement, CloseHandler on_close,
              FullscreenChangeHandler on_fullscreen_change = nullptr,
              PlacementChangeHandler on_placement_change = nullptr);
  void Destroy();

  HWND hwnd() const { return hwnd_; }

  // 이하 스레드 안전 (PostMessage 기반)
  void SetFullscreen(bool fullscreen);
  bool IsFullscreen() const { return fullscreen_; }
  void SetBackgroundColor(uint32_t rgb);  // 0xRRGGBB — 비디오 없을 때의 창 배경
  void Invalidate();  // 클라이언트 영역 다시 칠하기 요청 (레터박스 띠를 배경색으로 갱신)
  void ApplyPlacement(const WindowPlacement& placement);  // 라이브 재배치 (모니터/좌표/크기)

  // 현재 배치된 클라이언트 영역 크기 (aspect-ratio 계산 등에 사용)
  int client_width() const { return client_width_; }
  int client_height() const { return client_height_; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void ThreadMain(WindowPlacement placement, HANDLE ready_event);
  void ApplyFullscreen(bool fullscreen);
  void ToggleFullscreenFromKey();  // F11 — 창 스레드에서 직접 호출됨
  void ApplyPlacementOnThread(const WindowPlacement& placement);
  RECT ResolvePlacementRect(const WindowPlacement& placement) const;  // 모니터 좌표 → 절대 클라이언트 rect
  WindowPlacement CurrentPlacement() const;  // 현재 창의 실제 배치(모니터 인덱스+클라이언트 좌표/크기)

  HWND hwnd_ = nullptr;
  std::thread thread_;
  std::atomic<bool> fullscreen_{false};
  std::atomic<uint32_t> bg_rgb_{0x000000};
  std::atomic<int> client_width_{1280};
  std::atomic<int> client_height_{720};
  RECT windowed_rect_{};  // 풀스크린 해제 시 복원용 (창 스레드에서만 접근)
  CloseHandler on_close_;
  FullscreenChangeHandler on_fullscreen_change_;
  PlacementChangeHandler on_placement_change_;
};

}  // namespace vp
