#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <winsock2.h>

namespace vp {

// 루프백 전용 NDJSON TCP 서버 (단일 클라이언트).
// 호스트(Node 제어부)가 유일한 클라이언트이며, 끊기면 재접속을 기다린다.
// on_line 콜백은 내부 수신 스레드에서 호출된다 — 호출자가 GLib 메인루프로 마샬링할 것.
class NdjsonServer {
 public:
  using LineHandler = std::function<void(std::string line)>;

  NdjsonServer() = default;
  ~NdjsonServer();

  NdjsonServer(const NdjsonServer&) = delete;
  NdjsonServer& operator=(const NdjsonServer&) = delete;

  // preferred_port에 바인드 시도, 사용 중이면 임시 포트로 폴백.
  // 성공 시 실제 바인드된 포트를 반환, 실패 시 0.
  uint16_t Start(uint16_t preferred_port, LineHandler on_line);
  void Stop();

  // 개행을 붙여 현재 클라이언트로 전송 (스레드 안전). 클라이언트 없으면 무시.
  void SendLine(const std::string& json_line);

  // 클라이언트 연결 여부 (스레드 안전) — ready 발송 타이밍 판단용
  bool HasClient();

 private:
  void AcceptLoop();

  SOCKET listen_sock_ = INVALID_SOCKET;
  SOCKET client_sock_ = INVALID_SOCKET;
  std::mutex send_mutex_;
  std::thread accept_thread_;
  std::atomic<bool> running_{false};
  LineHandler on_line_;
};

}  // namespace vp
