# gstasio (vendored)

GStreamer **gst-plugins-bad 1.26.11**의 `sys/asio` 플러그인 소스 그대로 (LGPL v2+, `COPYING.LIB`).
Copyright (C) 2021 Seungha Yang / GStreamer 프로젝트.

- 공식 Windows 바이너리에 포함되지 않아 직접 빌드해 번들한다 (CMake 타깃 `gstasio`).
- 1.20+부터 ASIO 인터페이스를 **자체 클린룸 헤더(`asio.h`)로 재구현** — Steinberg SDK 불필요.
- **설치된 GStreamer를 업그레이드하면 동일 버전의 gst-plugins-bad 소스로 재-vendor할 것**
  (출처: https://gstreamer.freedesktop.org/src/gst-plugins-bad/).

"ASIO is a trademark of Steinberg Media Technologies GmbH."
