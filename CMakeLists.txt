cmake_minimum_required(VERSION 3.28)
project(vplayer VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- GStreamer (공식 MSVC devel MSI 필요) -----------------------------------
# GSTREAMER_1_0_ROOT_MSVC_X86_64 환경변수(MSI 설치 시 자동 설정)로 pkg-config 구성
# 환경변수가 없어도 기본 설치 경로에서 GStreamer를 찾는다 (캐시 오염 방지: FORCE)
set(_GST_CANDIDATES "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}"
                    "C:/Program Files/gstreamer/1.0/msvc_x86_64" "C:/gstreamer/1.0/msvc_x86_64")
foreach(_cand IN LISTS _GST_CANDIDATES)
  if(_cand)
    file(TO_CMAKE_PATH "${_cand}" _cand_cmake)
    if(EXISTS "${_cand_cmake}/lib/pkgconfig/gstreamer-1.0.pc")
      set(GST_ROOT "${_cand_cmake}")
      break()
    endif()
  endif()
endforeach()
if(GST_ROOT)
  set(ENV{PKG_CONFIG_PATH} "${GST_ROOT}/lib/pkgconfig")
  if(EXISTS "${GST_ROOT}/bin/pkg-config.exe")
    set(PKG_CONFIG_EXECUTABLE "${GST_ROOT}/bin/pkg-config.exe" CACHE FILEPATH "" FORCE)
  endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0
  gstreamer-video-1.0
  gstreamer-audio-1.0
  gstreamer-pbutils-1.0
  gstreamer-app-1.0
  gstreamer-net-1.0
)

# --- 서드파티 (FetchContent) ---------------------------------------------------
include(FetchContent)
# CACHE FORCE: lunasvg 등 서브프로젝트의 option(BUILD_SHARED_LIBS ON)을 이기려면 캐시 변수여야 함
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
# 로고 래스터라이즈: SVG = lunasvg, PNG/JPG = stb_image
FetchContent_Declare(lunasvg
  GIT_REPOSITORY https://github.com/sammycage/lunasvg.git
  GIT_TAG v2.4.1
  GIT_SHALLOW TRUE
)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json lunasvg stb)

# --- vplayer ------------------------------------------------------------------
add_executable(vplayer
  src/main.cpp
  src/net/tcp_server.cpp
  src/player/player_core.cpp
  src/player/player_timeline.cpp
  src/video/video_window.cpp
  src/video/display_enum.cpp
  src/media/probe.cpp
  src/app.manifest
)

target_include_directories(vplayer PRIVATE src "${stb_SOURCE_DIR}")
target_link_libraries(vplayer PRIVATE
  PkgConfig::GST
  nlohmann_json::nlohmann_json
  lunasvg
  ws2_32
  psapi
)

target_compile_options(vplayer PRIVATE /utf-8 /W4 /EHsc)
target_compile_definitions(vplayer PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE _UNICODE
  LUNASVG_BUILD_STATIC  # lunasvg 정적 링크 (dllimport 데코 방지 — 그들의 define이 PRIVATE라 소비자가 직접 지정)
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  # ASan은 GStreamer DLL과의 호환 이슈가 있으면 끄고 진행 (플랜 리스크 3 참고)
  # target_compile_options(vplayer PRIVATE /fsanitize=address)
endif()

# --- A0 스파이크: 믹서 N채널 라이브 전환 검증 (배포 미포함, 개발 전용) ----------
add_executable(spike_mixcaps EXCLUDE_FROM_ALL test/spike_mixcaps.cpp)
target_link_libraries(spike_mixcaps PRIVATE PkgConfig::GST)
target_compile_options(spike_mixcaps PRIVATE /utf-8 /W4 /EHsc)
target_compile_definitions(spike_mixcaps PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# --- gstasio 플러그인 (third_party/gstasio 참조 — vendored, LGPL) ---------------
# 공식 바이너리 미포함 플러그인을 함께 빌드해 번들. ASIO 디바이스 열람/출력용 (Phase 3 16ch).
add_library(gstasio MODULE
  third_party/gstasio/gstasiodeviceprovider.cpp
  third_party/gstasio/gstasioobject.cpp
  third_party/gstasio/gstasioringbuffer.cpp
  third_party/gstasio/gstasiosink.cpp
  third_party/gstasio/gstasiosrc.cpp
  third_party/gstasio/gstasioutils.cpp
  third_party/gstasio/plugin.c
)
set_target_properties(gstasio PROPERTIES PREFIX "")  # gstasio.dll (lib 접두어 방지)
target_include_directories(gstasio PRIVATE third_party/gstasio)
target_link_libraries(gstasio PRIVATE PkgConfig::GST avrt ole32)
target_compile_options(gstasio PRIVATE /utf-8 /EHsc /FS)  # /FS: 병렬 빌드 PDB 충돌 방지
target_compile_definitions(gstasio PRIVATE
  # WIN32_LEAN_AND_MEAN 금지 — 플러그인이 windows.h 경유 OLE/COM 선언(CLSIDFromString 등)에 의존
  PACKAGE="asio"
  VERSION="1.26.11"
  GST_PACKAGE_NAME="gst-plugins-bad-vendored"
  GST_PACKAGE_ORIGIN="https://gstreamer.freedesktop.org"
)

# 실행 시 GStreamer bin의 DLL을 찾도록 개발 환경에서는 PATH에 %GSTREAMER%\bin 필요.
# 배포 시에는 scripts/의 번들링 단계가 필요 DLL/플러그인을 gst-bundle/로 복사(추후 작성).
