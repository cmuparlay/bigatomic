// An implementation of big_atomic using a SeqLock.
//
//  Supports:
//  - Blocking loads (loads never obstruct each-other, but can be indefinitely blocked by writers)
//  - Blocking stores
//  - Blocking CAS
//
// No additional space usage
//

#ifndef PARLAYATOMIC_H_
#define PARLAYATOMIC_H_

#include <atomic>
#include <functional>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include <common/lock.h>

namespace parlay {

template<typename V, class KeyEqual = std::equal_to<V>>
struct big_atomic {

  using vtype = long;

  std::atomic<vtype> version;
  V val;

  big_atomic(const V& v) : version(0), val(v) {}
  big_atomic() : version(0) {}

  using tag = vtype;

  PARLAY_INLINE
  void store_sequential(const V& v) { val = v; }

  PARLAY_INLINE
  std::pair<V,tag> ll() {
    while (true) {
      int delay = 100;
      vtype ver = version.load(std::memory_order_acquire);
      V v = val;
      std::atomic_thread_fence(std::memory_order_acquire);
      if ((ver & 1) == 0 && version.load(std::memory_order_relaxed) == ver)
	return std::pair(v,ver);
      for (volatile int i = 0; i < delay; i++);
      delay = std::min(2 * delay, 1000);
    }
  }

  PARLAY_INLINE
  bool lv(tag tg) {
    return version.load() == tg;
  }

  PARLAY_INLINE
  bool sc(tag expected_tag, const V& v) {
    bool result = true;
    int delay = 100;
    while (true) {
      vtype ver = version.load();
      if (ver != expected_tag) return false;
      if (get_locks().try_lock((long)this, [&] {
            if (version.load(std::memory_order_acquire) != expected_tag)
              result = false;
            else {
              version.store(ver + 1, std::memory_order_relaxed);
              std::atomic_thread_fence(std::memory_order_release);
              val = v;
              version.store(ver + 2, std::memory_order_release);
            }
            return true;
          }))
        return result;
      for (volatile int i = 0; i < delay; i++);
      delay = std::min(2 * delay, 2000);
    }
  }

  PARLAY_INLINE V load() { return ll().first; }

  PARLAY_INLINE bool cas(const V& expected, const V& desired) {
    auto [current, tag] = ll();
    if (!KeyEqual{}(current, expected)) return false;
    if (KeyEqual{}(expected, desired)) return true;
    return sc(tag, desired);
  }

};

}  // namespace parlay
#endif  // PARLAYATOMIC_H_
