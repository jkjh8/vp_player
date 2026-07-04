# vp_player

네트워크 제어 비디오 플레이어 **vplayer.exe** — C++20 + GStreamer 1.26 기반 네이티브 미디어 엔진.

vp_app2(Node 제어부)가 spawn하여 루프백 TCP(개행 구분 JSON)로 제어하는 플레이어 프로세스입니다.
기존 Python(PySide6 + python-vlc) 플레이어의 드롭인 대체품이며, 이후 타임라인 재생·멀티채널(16ch+)·ASIO 출력으로 확장됩니다.

## 아키텍처 개요

```
Node 제어부 (vp_app2) ── loopback TCP 127.0.0.1:1300, NDJSON ──▶ vplayer.exe
                                                                  ├─ 덱 A/B (uridecodebin3) ─▶ d3d11compositor ─▶ d3d11videosink (자체 Win32 창)
                                                                  ├─ 오디오 ─▶ audiomixer ─▶ wasapi2sink / asiosink
                                                                  ├─ 이미지 타이머 / 로고 오버레이 / 배경색
                                                                  └─ 100ms player_data 틱
```

- 프로토콜 명세: [docs/PROTOCOL.md](docs/PROTOCOL.md)
- 전체 전환 계획·설계 결정 기록: vp_app2 저장소와 함께 관리되는 플랜 문서 참조

## 빌드 요구사항

| 도구 | 버전 | 비고 |
|---|---|---|
| Visual Studio 2022 | 17.x (C++ 데스크톱 워크로드) | MSVC 컴파일러 |
| CMake | ≥ 3.28 | |
| GStreamer MSVC | 1.26.x | **runtime + development MSI 둘 다** 설치 |

### GStreamer 설치 (최초 1회)

https://gstreamer.freedesktop.org/download/ 에서 **MSVC 64-bit** 패키지 2개 설치:

1. `gstreamer-1.0-msvc-x86_64-1.26.x.msi` (runtime)
2. `gstreamer-1.0-devel-msvc-x86_64-1.26.x.msi` (development)

설치 시 **Complete** 선택 권장. 설치 후 환경변수 `GSTREAMER_1_0_ROOT_MSVC_X86_64`가 잡히는지 확인
(기본: `C:\gstreamer\1.0\msvc_x86_64\`). CMake가 이 변수로 pkg-config 경로를 자동 구성합니다.

## 빌드

```powershell
cmake --preset vs2022
cmake --build --preset debug     # 또는 --preset release
```

산출물: `build/Debug/vplayer.exe`

## 실행 (단독 테스트)

```powershell
# 플레이어 실행 → stdout에 {"type":"port","data":{"port":1300}} 출력 후 대기
.\build\Debug\vplayer.exe

# 다른 터미널에서 명령 주입 (예)
# {"command":"playid","file":{"path":"C:\\media\\test.mp4"}}
# {"command":"stop"}
```

vp_app2와 연동 시에는 `VP_PLAYER_ENGINE=native`로 spawn 경로가 분기됩니다 (vp_app2 측 Phase 1 변경).

## 저장소 구조

```
src/
  main.cpp            엔트리: gst 초기화, 포트 핸드셰이크, GLib 메인루프, 명령 디스패치
  net/tcp_server.*    루프백 NDJSON TCP 서버 (단일 클라이언트)
  util/gst_ptr.h      GstObject RAII 래퍼
docs/
  PROTOCOL.md         제어 프로토콜 명세 (구현 계약)
scripts/              (예정) GStreamer 플러그인 번들링, ASIO 플러그인 빌드
```

## 로드맵 (요약)

- [x] 스파이크: 재생 + NDJSON 서버 + 100ms 틱 + 한글 파일명
- [x] 듀얼 덱 + d3d11compositor 갭리스 전환 (A/B 스왑 PASS)
- [x] 이미지 스틸/타이머, 로고(PNG+SVG), 배경색, 풀스크린
- [x] 오디오 디바이스(wasapi2) 열람 + 라이브 전환, 레거시 트랙 경로(previous/playlist_play)
- [x] vp_app2 실연동 E2E (VP_PLAYER_ENGINE=native)
- [x] 자립 번들 (`scripts\bundle.ps1` → dist/player ~45MB, GStreamer 미설치 환경 검증)
- [ ] ASIO 플러그인 빌드 (gstasio.dll — Steinberg SDK 필요)
- [ ] vp_app2 electron-builder 패키징 통합 + 설치본 크기 측정
- [ ] 트랜스크립트 리플레이 테스트
- [ ] (Phase 3) 타임라인 스케줄러, 멀티채널 mix-matrix(WASAPI 8ch / ASIO 드라이버 채널수), 독립 오디오 트랙
