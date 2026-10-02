#include "video/display_enum.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace vp {

namespace {

std::string Utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string s(static_cast<size_t>(len) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
  return s;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam) {
  auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

  MonitorInfo info;
  info.device_name = Utf8(mi.szDevice);
  info.x = mi.rcMonitor.left;
  info.y = mi.rcMonitor.top;
  info.width = mi.rcMonitor.right - mi.rcMonitor.left;
  info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
  info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  out->push_back(info);
  return TRUE;
}

// EDID 디스크립터(0xFF = 시리얼 문자열) → 없으면 헤더 32비트 시리얼 숫자.
std::string SerialFromEdid(const BYTE* e, DWORD n) {
  if (n < 128) return {};
  for (int d = 54; d + 18 <= 126; d += 18) {
    if (e[d] == 0 && e[d + 1] == 0 && e[d + 3] == 0xFF) {
      std::string s(reinterpret_cast<const char*>(e + d + 5), 13);
      const auto end = s.find_first_of("\n\r");
      if (end != std::string::npos) s.resize(end);
      while (!s.empty() && s.back() == ' ') s.pop_back();
      if (!s.empty()) return s;
    }
  }
  const DWORD num = e[12] | (e[13] << 8) | (e[14] << 16) | (static_cast<DWORD>(e[15]) << 24);
  return num ? std::to_string(num) : std::string();
}

// monitorDevicePath(\?\DISPLAY#MODEL#INSTANCE#{guid}) → 레지스트리 Enum\DISPLAY\MODEL\INSTANCE
// \Device Parameters\EDID 에서 시리얼. 경로별 캐시(주기 폴링이 레지스트리를 매번 읽지 않도록).
std::string ReadSerial(const std::wstring& device_path) {
  static std::mutex mu;
  static std::map<std::wstring, std::string> cache;
  std::lock_guard<std::mutex> lock(mu);
  if (auto it = cache.find(device_path); it != cache.end()) return it->second;

  std::string serial;
  std::wstring p = device_path;
  if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
  if (auto g = p.rfind(L"#{"); g != std::wstring::npos) p.resize(g);
  std::replace(p.begin(), p.end(), L'#', L'\\');  // DISPLAY\MODEL\INSTANCE
  const std::wstring sub = L"SYSTEM\\CurrentControlSet\\Enum\\" + p + L"\\Device Parameters";
  BYTE edid[512];
  DWORD size = sizeof(edid);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, edid,
                   &size) == ERROR_SUCCESS)
    serial = SerialFromEdid(edid, size);
  cache[device_path] = serial;
  return serial;
}

// GDI 장치명(\.\DISPLAYn) → {monitorDevicePath, 표시 이름, 시리얼}. 활성 경로만.
struct TargetIds {
  std::string key, name, serial;
};
std::map<std::string, TargetIds> QueryTargets() {
  std::map<std::string, TargetIds> out;
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return out;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) !=
      ERROR_SUCCESS)
    return out;
  paths.resize(np);
  for (const auto& path : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = path.sourceInfo.adapterId;
    src.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
    tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tgt.header.size = sizeof(tgt);
    tgt.header.adapterId = path.targetInfo.adapterId;
    tgt.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
    const std::string gdi = Utf8(src.viewGdiDeviceName);
    if (gdi.empty() || out.count(gdi)) continue;  // 복제(미러) 모드는 첫 타깃만
    out[gdi] = {Utf8(tgt.monitorDevicePath), Utf8(tgt.monitorFriendlyDeviceName),
                ReadSerial(tgt.monitorDevicePath)};
  }
  return out;
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
  const auto targets = QueryTargets();
  for (size_t i = 0; i < monitors.size(); i++) {
    auto& m = monitors[i];
    m.index = static_cast<int>(i);
    if (auto it = targets.find(m.device_name); it != targets.end()) {
      m.key = it->second.key;
      m.name = it->second.name;
      m.serial = it->second.serial;
    }
  }
  return monitors;
}

const MonitorInfo* FindMonitorByKey(const std::vector<MonitorInfo>& monitors,
                                    const std::string& key, const std::string& serial) {
  if (!key.empty()) {
    for (const auto& m : monitors)
      if (m.key == key) return &m;
  }
  // 폴백: 케이블 포트가 바뀌면 key(인스턴스 경로)가 바뀐다 → EDID 시리얼로 같은 모니터 탐색.
  // 시리얼이 겹치는(시리얼 미기록 모델 등) 경우는 오배치 위험이 있어 매칭하지 않는다.
  if (!serial.empty()) {
    const MonitorInfo* hit = nullptr;
    for (const auto& m : monitors) {
      if (m.serial != serial) continue;
      if (hit) return nullptr;
      hit = &m;
    }
    return hit;
  }
  return nullptr;
}

}  // namespace vp
