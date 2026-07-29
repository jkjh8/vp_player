#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <thread>

namespace vp {

// 시스템 트레이 아이콘 — "실행 중" 표시 + 우클릭 메뉴로 프로그램 종료.
// 자체 스레드에서 숨은 창 + 메시지 펌프를 돌린다(Shell_NotifyIcon 콜백 수신용).
// 종료 메뉴 선택 시 on_quit(트레이 스레드에서 호출 — 호출자가 메인루프로 마샬링할 것).
class TrayIcon {
 public:
  using QuitHandler = std::function<void()>;

  TrayIcon() = default;
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  bool Create(const std::wstring& tooltip, QuitHandler on_quit);
  void Destroy();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  void ThreadMain(std::wstring tooltip, HANDLE ready);

  std::thread thread_;
  HWND hwnd_ = nullptr;
  QuitHandler on_quit_;
};

}  // namespace vp
