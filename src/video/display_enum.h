#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace vp {

struct MonitorInfo {
  int index = 0;             // 안정적 식별자 (primary 우선 + 좌표순 정렬로 매 호출 순서 고정)
  std::string device_name;   // 예: \\.\DISPLAY1
  int x = 0, y = 0;          // 가상 데스크톱 좌표계 원점
  int width = 0, height = 0;
  bool primary = false;
};

// EnumDisplayMonitors 기반 모니터 목록 조회. 항상 primary가 index 0.
std::vector<MonitorInfo> EnumerateMonitors();

}  // namespace vp
