cmake_minimum_required(VERSION 3.28)
project(vplayer VERSION 0.1.0 LANGUAGES CXX)

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
)

# --- nlohmann/json (header-only, FetchContent) --------------------------------
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

# --- vplayer ------------------------------------------------------------------
add_executable(vplayer
  src/main.cpp
  src/net/tcp_server.cpp
  src/player/player_core.cpp
  src/video/video_window.cpp
  src/app.manifest
)

target_include_directories(vplayer PRIVATE src)
target_link_libraries(vplayer PRIVATE
  PkgConfig::GST
  nlohmann_json::nlohmann_json
  ws2_32
)

target_compile_options(vplayer PRIVATE /utf-8 /W4 /EHsc)
target_compile_definitions(vplayer PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE _UNICODE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  # ASan은 GStreamer DLL과의 호환 이슈가 있으면 끄고 진행 (플랜 리스크 3 참고)
  # target_compile_options(vplayer PRIVATE /fsanitize=address)
endif()

# 실행 시 GStreamer bin의 DLL을 찾도록 개발 환경에서는 PATH에 %GSTREAMER%\bin 필요.
# 배포 시에는 scripts/의 번들링 단계가 필요 DLL/플러그인을 gst-bundle/로 복사(추후 작성).
