// Minimal std::atomic_ref backfill for libc++ 16 (LLVM 16), which does not
// provide std::atomic_ref. Force-included (via -include) into the dartvm build
// only. Implements the subset of the C++20 std::atomic_ref API that the Dart VM
// runtime uses. Backed by compiler __atomic_* builtins.
#pragma once

#include <atomic>
#include <type_traits>
#include <version>

// If the standard library already provides std::atomic_ref (libc++ >= 18, any
// recent libstdc++), do nothing. libc++ 16 does not define this feature macro.
#if !defined(__cpp_lib_atomic_ref)

namespace std {

template <typename T>
struct atomic_ref {
  static_assert(std::is_trivially_copyable<T>::value,
                "std::atomic_ref requires a trivially copyable type");

  explicit atomic_ref(T& obj) noexcept : ptr_(&obj) {}
  atomic_ref(const atomic_ref&) noexcept = default;
  atomic_ref& operator=(const atomic_ref&) = delete;

  T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    T ret;
    __atomic_load(ptr_, &ret, static_cast<int>(order));
    return ret;
  }

  void store(T desired,
             std::memory_order order = std::memory_order_seq_cst) noexcept {
    __atomic_store(ptr_, &desired, static_cast<int>(order));
  }

  T exchange(T desired,
             std::memory_order order = std::memory_order_seq_cst) noexcept {
    T ret;
    __atomic_exchange(ptr_, &desired, &ret, static_cast<int>(order));
    return ret;
  }

  bool compare_exchange_weak(T& expected, T desired, std::memory_order success,
                             std::memory_order failure) noexcept {
    return __atomic_compare_exchange(ptr_, &expected, &desired, true,
                                     static_cast<int>(success),
                                     static_cast<int>(failure));
  }

  bool compare_exchange_weak(
      T& expected, T desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept {
    return compare_exchange_weak(expected, desired, order, order);
  }

  bool compare_exchange_strong(T& expected, T desired, std::memory_order success,
                               std::memory_order failure) noexcept {
    return __atomic_compare_exchange(ptr_, &expected, &desired, false,
                                     static_cast<int>(success),
                                     static_cast<int>(failure));
  }

  bool compare_exchange_strong(
      T& expected, T desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept {
    return compare_exchange_strong(expected, desired, order, order);
  }

  T fetch_add(T arg,
              std::memory_order order = std::memory_order_seq_cst) noexcept {
    return __atomic_fetch_add(ptr_, arg, static_cast<int>(order));
  }

  T fetch_sub(T arg,
              std::memory_order order = std::memory_order_seq_cst) noexcept {
    return __atomic_fetch_sub(ptr_, arg, static_cast<int>(order));
  }

  T fetch_and(T arg,
              std::memory_order order = std::memory_order_seq_cst) noexcept {
    return __atomic_fetch_and(ptr_, arg, static_cast<int>(order));
  }

  T fetch_or(T arg,
             std::memory_order order = std::memory_order_seq_cst) noexcept {
    return __atomic_fetch_or(ptr_, arg, static_cast<int>(order));
  }

  T fetch_xor(T arg,
              std::memory_order order = std::memory_order_seq_cst) noexcept {
    return __atomic_fetch_xor(ptr_, arg, static_cast<int>(order));
  }

 private:
  T* ptr_;
};

}  // namespace std

#endif  // !defined(__cpp_lib_atomic_ref)
