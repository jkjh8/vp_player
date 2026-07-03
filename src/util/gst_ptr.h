#pragma once

#include <gst/gst.h>

#include <utility>

namespace vp {

// GstObject/GObject 계열의 unref-on-scope-exit RAII 래퍼.
// GStreamer C API의 참조 카운팅 실수(가장 흔한 버그 유형)를 구조적으로 차단한다.
template <typename T>
class GstPtr {
 public:
  GstPtr() = default;
  // 소유권을 넘겨받는다 (transfer full). floating ref는 sink 처리.
  explicit GstPtr(T* ptr) : ptr_(ptr) {
    if (ptr_ && g_object_is_floating(ptr_)) {
      g_object_ref_sink(ptr_);
    }
  }
  ~GstPtr() { reset(); }

  GstPtr(const GstPtr&) = delete;
  GstPtr& operator=(const GstPtr&) = delete;

  GstPtr(GstPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
  GstPtr& operator=(GstPtr&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = std::exchange(other.ptr_, nullptr);
    }
    return *this;
  }

  void reset(T* ptr = nullptr) {
    if (ptr_) gst_object_unref(GST_OBJECT(ptr_));
    ptr_ = ptr;
  }

  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

  // C API에 넘길 때 소유권을 반환 (transfer full)
  T* release() { return std::exchange(ptr_, nullptr); }

 private:
  T* ptr_ = nullptr;
};

}  // namespace vp
