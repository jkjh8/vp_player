#include "video/display_enum.h"

#include <algorithm>

namespace vp {

namespace {

BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam) {
  auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

  MonitorInfo info;
  const int len =
      WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, nullptr, 0, nullptr, nullptr);
  if (len > 0) {
    info.device_name.resize(static_cast<size_t>(len) - 1);
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, info.device_name.data(), len, nullptr,
                        nullptr);
  }
  info.x = mi.rcMonitor.left;
  info.y = mi.rcMonitor.top;
  info.width = mi.rcMonitor.right - mi.rcMonitor.left;
  info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
  info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  out->push_back(info);
  return TRUE;
}

}  // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
  std::vector<MonitorInfo> monitors;
  EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));

  // primary 우선 + 좌표순으로 정렬해 매 호출마다 동일한 index가 나오도록 안정화
  std::stable_sort(monitors.begin(), monitors.end(),
                    [](const MonitorInfo& a, const MonitorInfo& b) {
                      if (a.primary != b.primary) return a.primary;
                      if (a.x != b.x) return a.x < b.x;
                      return a.y < b.y;
                    });
  for (size_t i = 0; i < monitors.size(); i++) monitors[i].index = static_cast<int>(i);
  return monitors;
}

}  // namespace vp
