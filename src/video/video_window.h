#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace vp {

// 비디오 출력용 자체 Win32 창.
// 별도 스레드에서 메시지 펌프를 돌리며(플랜의 Thread A), 창 조작은 전부
// PostMessage로 창 스레드에 마샬링된다. HWND는 d3d11videosink의
// GstVideoOverlay에 넘겨진다.
class VideoWindow {
 public:
  // 사용자가 창을 닫을 때 호출 (창 스레드에서 호출됨 — 호출자가 마샬링할 것)
  using CloseHandler = std::function<void()>;

  VideoWindow() = default;
  ~VideoWindow();

  VideoWindow(const VideoWindow&) = delete;
  VideoWindow& operator=(const VideoWindow&) = delete;

  // 창 생성 (블로킹 — 생성 완료 후 반환). 성공 시 true.
  bool Create(int width, int height, CloseHandler on_close);
  void Destroy();

  HWND hwnd() const { return hwnd_; }

  // 이하 스레드 안전 (PostMessage 기반)
  void SetFullscreen(bool fullscreen);
  bool IsFullscreen() const { return fullscreen_; }
  void SetBackgroundColor(uint32_t rgb);  // 0xRRGGBB — 비디오 없을 때의 창 배경

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void ThreadMain(int width, int height, HANDLE ready_event);
  void ApplyFullscreen(bool fullscreen);

  HWND hwnd_ = nullptr;
  std::thread thread_;
  std::atomic<bool> fullscreen_{false};
  std::atomic<uint32_t> bg_rgb_{0x000000};
  RECT windowed_rect_{};  // 풀스크린 해제 시 복원용 (창 스레드에서만 접근)
  CloseHandler on_close_;
};

}  // namespace vp
