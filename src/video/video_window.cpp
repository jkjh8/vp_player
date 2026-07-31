#include "video/video_window.h"

#include "video/display_enum.h"

namespace vp {

namespace {
constexpr wchar_t kClassName[] = L"vplayerVideoWindow";
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 1;  // wParam: 0/1
constexpr UINT WM_APP_DESTROY = WM_APP + 2;
constexpr UINT WM_APP_PLACEMENT = WM_APP + 3;  // lParam: WindowPlacement* (수신측이 delete)
// 사이니지 출력창: 제목 표시줄/테두리 없는 borderless (비-전체화면 상태에서도). 배치는
// 호스트 명령(set_display)으로만 하므로 캡션/리사이즈 프레임이 필요 없다. WS_POPUP은
// 프레임이 없어 창 rect == 클라이언트 rect (AdjustWindowRect는 사실상 no-op).
constexpr DWORD kWindowedStyle = WS_POPUP;
}  // namespace

VideoWindow::~VideoWindow() { Destroy(); }

RECT VideoWindow::ResolvePlacementRect(const WindowPlacement& placement) const {
  const auto monitors = EnumerateMonitors();
  MonitorInfo mon;
  bool found = false;
  for (const auto& m : monitors) {
    if (m.index == placement.monitor_index) {
      mon = m;
      found = true;
      break;
    }
  }
  if (!found) {
    for (const auto& m : monitors) {
      if (m.primary) {
        mon = m;
        found = true;
        break;
      }
    }
  }
  if (!found && !monitors.empty()) mon = monitors.front();

  const int w = placement.width > 0 ? placement.width : mon.width;
  const int h = placement.height > 0 ? placement.height : mon.height;
  return RECT{mon.x + placement.x, mon.y + placement.y, mon.x + placement.x + w,
              mon.y + placement.y + h};
}

WindowPlacement VideoWindow::CurrentPlacement() const {
  WindowPlacement p;
  if (!hwnd_) return p;

  // 클라이언트 영역의 화면 좌표 + 크기
  RECT cr{};
  GetClientRect(hwnd_, &cr);
  POINT tl{0, 0};
  ClientToScreen(hwnd_, &tl);
  const int client_w = cr.right - cr.left;
  const int client_h = cr.bottom - cr.top;

  // 창이 걸쳐 있는 모니터를 찾아 인덱스/원점 확정 (EnumerateMonitors와 동일 정렬 규칙)
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);

  const auto monitors = EnumerateMonitors();
  int mon_x = mi.rcMonitor.left, mon_y = mi.rcMonitor.top;
  p.monitor_index = -1;
  for (const auto& m : monitors) {
    if (m.x == mi.rcMonitor.left && m.y == mi.rcMonitor.top) {
      p.monitor_index = m.index;
      mon_x = m.x;
      mon_y = m.y;
      break;
    }
  }
  p.x = tl.x - mon_x;
  p.y = tl.y - mon_y;
  p.width = client_w;
  p.height = client_h;
  return p;
}

bool VideoWindow::Create(const WindowPlacement& placement, CloseHandler on_close,
                          FullscreenChangeHandler on_fullscreen_change,
                          PlacementChangeHandler on_placement_change) {
  on_close_ = std::move(on_close);
  on_fullscreen_change_ = std::move(on_fullscreen_change);
  on_placement_change_ = std::move(on_placement_change);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread(&VideoWindow::ThreadMain, this, placement, ready);
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

void VideoWindow::ApplyPlacement(const WindowPlacement& placement) {
  if (!hwnd_) return;
  auto* copy = new WindowPlacement(placement);
  PostMessageW(hwnd_, WM_APP_PLACEMENT, 0, reinterpret_cast<LPARAM>(copy));
}

void VideoWindow::ApplyPlacementOnThread(const WindowPlacement& placement) {
  const RECT target = ResolvePlacementRect(placement);
  client_width_ = target.right - target.left;
  client_height_ = target.bottom - target.top;

  RECT outer = target;
  AdjustWindowRect(&outer, kWindowedStyle, FALSE);
  windowed_rect_ = outer;  // 풀스크린 해제 시 복귀할 좌표

  if (fullscreen_) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    SetWindowPos(hwnd_, nullptr, outer.left, outer.top, outer.right - outer.left,
                 outer.bottom - outer.top, SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void VideoWindow::ThreadMain(WindowPlacement placement, HANDLE ready_event) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // WM_ERASEBKGND에서 직접 칠함
  wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));  // 앱 아이콘(app.rc)
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  const RECT target = ResolvePlacementRect(placement);
  client_width_ = target.right - target.left;
  client_height_ = target.bottom - target.top;

  RECT rect = target;
  AdjustWindowRect(&rect, kWindowedStyle, FALSE);
  hwnd_ = CreateWindowExW(0, kClassName, L"VP Player", kWindowedStyle | WS_VISIBLE,
                          rect.left, rect.top, rect.right - rect.left,
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
    SetWindowLongPtrW(hwnd_, GWL_STYLE, kWindowedStyle | WS_VISIBLE);
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
    case WM_APP_PLACEMENT: {
      auto* placement = reinterpret_cast<WindowPlacement*>(lp);
      if (self) self->ApplyPlacementOnThread(*placement);
      delete placement;
      return 0;
    }
    case WM_SIZE:
      // 클라이언트 크기 추적 (crop cover 계산·aspect 갱신용). 풀스크린/프로그램 배치도 포함.
      if (self && wp != SIZE_MINIMIZED) {
        self->client_width_ = LOWORD(lp);
        self->client_height_ = HIWORD(lp);
      }
      break;
    case WM_EXITSIZEMOVE:
      // 사용자가 창을 직접 이동/리사이즈한 뒤 놓는 시점 — 실제 배치를 호스트로 역보고.
      // 풀스크린 상태(WS_POPUP)에서는 사용자 조작 배치가 무의미하므로 제외.
      if (self && self->on_placement_change_ && !self->fullscreen_) {
        self->on_placement_change_(self->CurrentPlacement());
      }
      break;
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
