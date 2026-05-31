#pragma once

#include <atomic>
#include <chrono>
#include <cstring>

#include <parlay/portability.h>

namespace parlay {

template <typename Time>
void spinDelay(Time duration) {
    auto start = std::chrono::high_resolution_clock::now();
    while (std::chrono::high_resolution_clock::now() - start < duration) {}
}
  
#ifdef __cpp_lib_hardware_interference_size

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"

inline constexpr std::size_t CACHE_LINE_ALIGNMENT = 2 * std::hardware_destructive_interference_size;

#pragma GCC diagnostic pop

#else
inline constexpr std::size_t CACHE_LINE_ALIGNMENT = 128;
#endif

// Pretend implementation of P1478R1: Byte-wise atomic memcpy. Technically undefined behavior
// since std::memcpy is not immune to data races, but on most hardware we should be okay. In
// C++26 we can probably do this for real (assuming Concurrency TS 2 is accepted).
PARLAY_INLINE void atomic_load_per_byte_memcpy(void* dest, const void* source, size_t count, std::memory_order order = std::memory_order_acquire) {
  std::memcpy(dest, source, count);
  std::atomic_thread_fence(order);
}

PARLAY_INLINE void atomic_store_per_byte_memcpy(void* dest, const void* source, size_t count, std::memory_order order = std::memory_order_release) {
  std::atomic_thread_fence(order);
  std::memcpy(dest, source, count);
}

#ifdef CPP20
// C++-20 compliant, but slower (source and dest must be word alligned, and count a multiple of 8)
PARLAY_INLINE void atomic_load_per_word_memcpy(void* dest, const void* source, size_t count,
                                               std::memory_order order = std::memory_order_acquire) {
  for (int64_t i = 0; i < count/8; i++) {
    std::atomic_ref<const int64_t> atomic_src(*(((int64_t*) source) + i));
    std::atomic_ref<int64_t> atomic_dest(*(((int64_t*) dest) + i));
    auto v = atomic_src.load(std::memory_order_relaxed);
    atomic_dest.store(v, std::memory_order_relaxed);
  }
  std::atomic_thread_fence(order);
}

    // C++ compliant, but slower
PARLAY_INLINE void atomic_store_per_word_memcpy(void* dest, const void* source, size_t count,
                                                std::memory_order order = std::memory_order_release) {
  std::atomic_thread_fence(order);
  for (int64_t i = 0; i < count/8; i++) {
    std::atomic_ref<const int64_t> atomic_src(*(((int64_t*) source) + i));
    std::atomic_ref<int64_t> atomic_dest(*(((int64_t*) dest) + i));
    auto v = atomic_src.load(std::memory_order_relaxed);
    atomic_dest.store(v, std::memory_order_relaxed);
  }
}
#endif
  
// Basically std::bit_cast from C++20 but with a slightly different interface. The goal
// is to type pun from a byte representation into a valid object with valid lifetime.
template<typename T>
PARLAY_INLINE T bits_to_object(const char* src) {
  if constexpr (!std::is_trivially_default_constructible_v<T>) {
    struct uninit_ {
      struct empty {};
      union {
        empty empty_;
        T value;
      };
      uninit_() : empty_{} {}
    } holder;
    std::memcpy(&holder.value, src, sizeof(T));
    return holder.value;
  } else {
    T value;
    std::memcpy(&value, src, sizeof(T));
    return value;
  }
}

}  // namespace parlay

