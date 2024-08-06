// An implementation of big_atomic using hardware transactional memory (HTM). This requires a CPU and
// compiler that supports HTM, i.e., an old CPU and probably GCC.
//

#ifndef PARLAYATOMIC_H_
#define PARLAYATOMIC_H_

#include <immintrin.h>

#include <cstddef>

#include <atomic>
#include <functional>
#include <mutex>

#include <parlay/portability.h>

#include <common/spinlock.h>

namespace parlay {

template <typename T, typename Equal = std::equal_to<T>>
struct big_atomic {

  constexpr static int max_transaction_attempts = 10;

  // T must be trivially copyable, but it doesn't have to be trivially default constructible (or
  // even default constructible at all) since we only make copies from what the user gives us
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(std::is_invocable_r_v<bool, Equal, T&&, T&&>);

  big_atomic() {
    static_assert(std::is_default_constructible_v<T>);
    new (static_cast<void*>(&value)) T{};
  }

  /* implicit */ big_atomic(const T& v) {   // NOLINT(google-explicit-constructor)
    new (static_cast<void*>(&value)) T{v};
  }

  ~big_atomic() = default;

  T load() {
    return with_transaction_or_lock([&]() { return value; });
  }

  void store(const T& new_v) {
    with_transaction_or_lock([&]() {
      value = new_v;
    });
  }

  bool cas(T expected_v, const T& new_v) {
    return with_transaction_or_lock([&]() {
      if (value == expected_v) {
        value = new_v;
        return true;
      }
      else {
        return false;
      }
    });
  }


  template<typename F>
  std::invoke_result_t<F> with_transaction_or_lock(F&& f) {
    static_assert(std::is_invocable_v<F>);

    int attempts = 0;
    unsigned status = _XABORT_EXPLICIT;

    for (; attempts < max_transaction_attempts; attempts++) {
      status = _xbegin();
      if (status == _XBEGIN_STARTED || !(status & _XABORT_RETRY))
        break;
    }

    if (status == _XBEGIN_STARTED) {
      if (lock.is_locked()) _xabort(0xA0);
      struct guard { ~guard() { _xend(); }} g_;
      return std::invoke(f);
    } else {
      std::lock_guard<spinlock> g{lock};
      return std::invoke(f);
    }
  }

  T value;
  spinlock lock;
};

} // namespace parlay

#endif //PARLAYATOMIC_H_
