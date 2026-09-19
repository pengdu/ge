#ifndef GE_CPP_ATOMIC_SHARED_PTR_H_
#define GE_CPP_ATOMIC_SHARED_PTR_H_

// Lock-free-ish atomic shared_ptr slot with one spelling on every toolchain.
//
// C++20 deprecates the free std::atomic_load/store(shared_ptr*) overloads in
// favour of std::atomic<std::shared_ptr<T>>; libstdc++ (GCC 12+) warns on
// them under -Werror while libc++ (Apple clang) still has no
// std::atomic<std::shared_ptr>. Pick whichever the standard library offers.

#include <atomic>
#include <memory>
#include <version>

namespace ge {

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L

template <typename T>
class AtomicSharedPtr final {
 public:
  AtomicSharedPtr() noexcept = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> p) noexcept : slot_(std::move(p)) {}
  AtomicSharedPtr(const AtomicSharedPtr&) = delete;
  AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

  [[nodiscard]] std::shared_ptr<T> load() const noexcept { return slot_.load(std::memory_order_acquire); }
  void store(std::shared_ptr<T> p) noexcept { slot_.store(std::move(p), std::memory_order_release); }

 private:
  std::atomic<std::shared_ptr<T>> slot_;
};

#else

template <typename T>
class AtomicSharedPtr final {
 public:
  AtomicSharedPtr() noexcept = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> p) noexcept : slot_(std::move(p)) {}
  AtomicSharedPtr(const AtomicSharedPtr&) = delete;
  AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

  [[nodiscard]] std::shared_ptr<T> load() const noexcept {
    return std::atomic_load_explicit(&slot_, std::memory_order_acquire);
  }
  void store(std::shared_ptr<T> p) noexcept { std::atomic_store_explicit(&slot_, std::move(p), std::memory_order_release); }

 private:
  std::shared_ptr<T> slot_;
};

#endif

}  // namespace ge

#endif
