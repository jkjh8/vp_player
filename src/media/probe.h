#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace vp {

// 미디어 프로브/썸네일 — ffmpeg-static 대체 (Phase 2.5).
// 재생 파이프라인과 독립된 워커 스레드에서 GStreamer(GstDiscoverer + 스냅샷
// 파이프라인)로 처리하므로 재생을 막지 않는다. 콜백은 워커 스레드에서 호출되며,
// NdjsonServer::SendLine이 스레드 안전하므로 그대로 피드백을 보내도 된다.

// 메타데이터 추출 → ffprobe 호환 형태({format, streams[]})로 콜백.
// 실패 시 json에 {error} 포함.
void ProbeMediaAsync(const std::string& path, std::function<void(nlohmann::json)> cb);

// 썸네일 PNG 생성: video는 at_sec 지점, image는 첫 프레임을 width로 스케일해 out에 저장.
void MakeThumbnailAsync(const std::string& path, const std::string& out, bool is_image,
                        double at_sec, int width, std::function<void(bool ok, std::string err)> cb);

}  // namespace vp
