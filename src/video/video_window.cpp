#include "video/video_window.h"

namespace vp {

namespace {
constexpr wchar_t kClassName[] = L"vplayerVideoWindow";
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 1;  // wParam: 0/1
constexpr UINT WM_APP_DESTROY = WM_APP + 2;
}  // namespace

VideoWindow::~VideoWindow() { Destroy(); }

bool VideoWindow::Create(int width, int height, CloseHandler on_close,
                          FullscreenChangeHandler on_fullscreen_change) {
  on_close_ = std::move(on_close);
  on_fullscreen_change_ = std::move(on_fullscreen_change);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread(&VideoWindow::ThreadMain, this, width, height, ready);
  WaitForSingleObject(ready, 5000);
  CloseHandle(ready);
  return hwnd_ != nullptr;
}

void VideoWindow::Destroy() {
  if (hwnd_) PostMessageW(hwnd_, WM_APP_DESTROY, 0, 0);
  if (thread_.joinable()) thread_.join();
  hwnd_ = nullptr;
}

void VideoWindow::SetFullscreen(bool fullscreen) {
  if (hwnd_) PostMessageW(hwnd_, WM_APP_FULLSCREEN, fullscreen ? 1 : 0, 0);
}

void VideoWindow::SetBackgroundColor(uint32_t rgb) {
  bg_rgb_ = rgb;
  if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void VideoWindow::ThreadMain(int width, int height, HANDLE ready_event) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // WM_ERASEBKGND에서 직접 칠함
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  RECT rect{0, 0, width, height};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  hwnd_ = CreateWindowExW(0, kClassName, L"VP Player", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                          rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, this);
  SetEvent(ready_event);
  if (!hwnd_) return;

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void VideoWindow::ToggleFullscreenFromKey() {
  const bool next = !fullscreen_;
  ApplyFullscreen(next);
  if (on_fullscreen_change_) on_fullscreen_change_(next);
}

void VideoWindow::ApplyFullscreen(bool fullscreen) {
  if (fullscreen == fullscreen_) return;
  fullscreen_ = fullscreen;

  if (fullscreen) {
    GetWindowRect(hwnd_, &windowed_rect_);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(hwnd_, HWND_NOTOPMOST, windowed_rect_.left, windowed_rect_.top,
                 windowed_rect_.right - windowed_rect_.left,
                 windowed_rect_.bottom - windowed_rect_.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }
}

LRESULT CALLBACK VideoWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<VideoWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return 0;
    }
    case WM_ERASEBKGND: {
      if (!self) break;
      const uint32_t rgb = self->bg_rgb_;
      HBRUSH brush = CreateSolidBrush(RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
      DeleteObject(brush);
      return 1;
    }
    case WM_APP_FULLSCREEN:
      if (self) self->ApplyFullscreen(wp != 0);
      return 0;
    case WM_SETCURSOR:
      // 클라이언트 영역(비디오 표시부) 위에서는 항상 커서 숨김 — 테두리/타이틀바 등
      // 비클라이언트 영역은 기본 처리(크기조절 커서 등)를 유지한다.
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_KEYDOWN:
      if (wp == VK_F11 && self) {
        self->ToggleFullscreenFromKey();
        return 0;
      }
      break;
    case WM_APP_DESTROY:
      DestroyWindow(hwnd);
      return 0;
    case WM_CLOSE:
      // 사용자가 창을 닫음 → 호스트에 closed 통지 (앱 종료로 이어짐)
      if (self && self->on_close_) self->on_close_();
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace vp
