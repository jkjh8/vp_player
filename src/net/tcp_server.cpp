#include "net/tcp_server.h"

#include <ws2tcpip.h>

namespace vp {

namespace {

SOCKET CreateListenSocket(uint16_t port, uint16_t* bound_port) {
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return INVALID_SOCKET;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    ::closesocket(s);
    return INVALID_SOCKET;
  }
  if (::listen(s, 1) == SOCKET_ERROR) {
    ::closesocket(s);
    return INVALID_SOCKET;
  }

  sockaddr_in actual{};
  int len = sizeof(actual);
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
    *bound_port = ntohs(actual.sin_port);
  }
  return s;
}

}  // namespace

NdjsonServer::~NdjsonServer() { Stop(); }

uint16_t NdjsonServer::Start(uint16_t preferred_port, LineHandler on_line) {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

  uint16_t bound = 0;
  listen_sock_ = CreateListenSocket(preferred_port, &bound);
  if (listen_sock_ == INVALID_SOCKET) {
    // 선호 포트 사용 중 → 임시 포트 폴백 (핸드셰이크로 실제 포트를 알리므로 안전)
    listen_sock_ = CreateListenSocket(0, &bound);
  }
  if (listen_sock_ == INVALID_SOCKET) return 0;

  on_line_ = std::move(on_line);
  running_ = true;
  accept_thread_ = std::thread(&NdjsonServer::AcceptLoop, this);
  return bound;
}

void NdjsonServer::Stop() {
  running_ = false;
  if (listen_sock_ != INVALID_SOCKET) {
    ::closesocket(listen_sock_);
    listen_sock_ = INVALID_SOCKET;
  }
  {
    std::lock_guard lock(send_mutex_);
    if (client_sock_ != INVALID_SOCKET) {
      ::closesocket(client_sock_);
      client_sock_ = INVALID_SOCKET;
    }
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  WSACleanup();
}

void NdjsonServer::SendLine(const std::string& json_line) {
  std::lock_guard lock(send_mutex_);
  if (client_sock_ == INVALID_SOCKET) return;
  std::string framed = json_line;
  framed.push_back('\n');
  size_t sent = 0;
  while (sent < framed.size()) {
    int n = ::send(client_sock_, framed.data() + sent, static_cast<int>(framed.size() - sent), 0);
    if (n == SOCKET_ERROR) return;  // 끊김은 수신 루프가 감지/처리
    sent += static_cast<size_t>(n);
  }
}

void NdjsonServer::AcceptLoop() {
  while (running_) {
    SOCKET client = ::accept(listen_sock_, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      if (!running_) break;
      continue;
    }
    {
      std::lock_guard lock(send_mutex_);
      client_sock_ = client;
    }

    std::string buffer;
    char chunk[4096];
    while (running_) {
      int n = ::recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) break;  // 연결 종료/오류 → 재접속 대기
      buffer.append(chunk, static_cast<size_t>(n));

      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && on_line_) on_line_(std::move(line));
      }
    }

    {
      std::lock_guard lock(send_mutex_);
      ::closesocket(client);
      if (client_sock_ == client) client_sock_ = INVALID_SOCKET;
    }
  }
}

}  // namespace vp
