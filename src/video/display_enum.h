#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace vp {

struct MonitorInfo {
  // 순번 (primary 우선 + 좌표순). 연결 구성이 바뀌면(늦게 켜지는 모니터 등) 밀리므로 영속 식별자로
  // 쓰지 말 것 — 영속 식별은 key(→ 실패 시 serial) 사용.
  int index = 0;
  std::string device_name;   // 예: \.\DISPLAY1 (재부팅/포트 순서로 바뀔 수 있음)
  // 물리 모니터 고정 식별자 = DisplayConfig monitorDevicePath
  // (예: \?\DISPLAY#GSM774F#4&1b70ca4a&0&UID28744#{e6f07b5f-...}). 같은 포트면 재부팅에도 불변.
  std::string key;
  std::string name;          // 모니터 표시 이름 (EDID, 예: "LG HDR 4K")
  std::string serial;        // EDID 시리얼 (케이블을 다른 포트로 옮겨 key가 바뀔 때 폴백 매칭)
  int x = 0, y = 0;          // 가상 데스크톱 좌표계 원점
  int width = 0, height = 0;
  bool primary = false;
};

// EnumDisplayMonitors 기반 모니터 목록 조회. 항상 primary가 index 0.
std::vector<MonitorInfo> EnumerateMonitors();

// 영속 식별자로 모니터 찾기: key 일치 → serial 일치(유일할 때) 순. 못 찾으면 nullptr.
const MonitorInfo* FindMonitorByKey(const std::vector<MonitorInfo>& monitors,
                                    const std::string& key, const std::string& serial);

}  // namespace vp
