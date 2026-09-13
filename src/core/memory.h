#pragma once
#include <mimalloc.h>
namespace kimix {
struct IOperatorNewBase {
  static void *operator new(size_t size) noexcept { return mi_malloc(size); }
  static void *operator new(size_t, void *place) noexcept { return place; }
  static void *operator new[](size_t size) noexcept { return mi_malloc(size); }
  static void *operator new(size_t size, const std::nothrow_t &) noexcept {
    return mi_malloc(size);
  }
  static void *operator new(size_t, void *place,
                            const std::nothrow_t &) noexcept {
    return place;
  }
  static void *operator new[](size_t size, const std::nothrow_t &) noexcept {
    return mi_malloc(size);
  }
  static void operator delete(void *pdead) noexcept { mi_free(pdead); }
  static void operator delete(void *ptr, void *place) noexcept {
    // do nothing
  }
  static void operator delete[](void *pdead) noexcept { mi_free(pdead); }
  static void operator delete(void *pdead, size_t) noexcept { mi_free(pdead); }
  static void operator delete[](void *pdead, size_t) noexcept {
    mi_free(pdead);
  }
};
} // namespace kimix