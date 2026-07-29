#include "video/tray_icon.h"

#include <shellapi.h>

namespace vp {

namespace {
constexpr wchar_t kClassName[] = L"vplayerTrayWnd";
constexpr UINT WM_APP_TRAY = WM_APP + 10;   // Shell_NotifyIcon 콜백
constexpr UINT WM_APP_TRAY_QUIT = WM_APP + 11;  // Destroy() 요청
constexpr UINT kTrayId = 1;
constexpr UINT kMenuQuit = 100;
}  // namespace

TrayIcon::~TrayIcon() { Destroy(); }

bool TrayIcon::Create(const std::wstring& tooltip, QuitHandler on_quit) {
  on_quit_ = std::move(on_quit);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread(&TrayIcon::ThreadMain, this, tooltip, ready);
  WaitForSingleObject(ready, 5000);
  CloseHandle(ready);
  return hwnd_ != nullptr;
}

void TrayIcon::Destroy() {
  if (hwnd_) PostMessageW(hwnd_, WM_APP_TRAY_QUIT, 0, 0);
  if (thread_.joinable()) thread_.join();
  hwnd_ = nullptr;
}

void TrayIcon::ThreadMain(std::wstring tooltip, HANDLE ready_event) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  // 보이지 않는 메시지 수신용 창 (Shell_NotifyIcon 콜백 대상)
  hwnd_ = CreateWindowExW(0, kClassName, L"vplayerTray", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                          wc.hInstance, this);
  if (!hwnd_) {
    SetEvent(ready_event);
    return;
  }

  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcsncpy_s(nid.szTip, tooltip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &nid);

  SetEvent(ready_event);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  Shell_NotifyIconW(NIM_DELETE, &nid);
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return 0;
    }
    case WM_APP_TRAY:
      // 우클릭 / 컨텍스트 메뉴 → 팝업 메뉴
      if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU ||
          LOWORD(lp) == WM_LBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuQuit, L"VP App 종료");
        SetForegroundWindow(hwnd);  // 메뉴가 포커스를 잃고 남지 않도록
        const int cmd = static_cast<int>(TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr));
        DestroyMenu(menu);
        if (cmd == kMenuQuit && self && self->on_quit_) self->on_quit_();
      }
      return 0;
    case WM_APP_TRAY_QUIT:
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
