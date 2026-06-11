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
#include <optional>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include <common/lock.h>
#include <common/utility.h>

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
    int delay = 500;
    while (true) {
      vtype ver = version.load(std::memory_order_acquire);
      V v = val;
      std::atomic_thread_fence(std::memory_order_acquire);
      if ((ver & 1) == 0 && version.load(std::memory_order_relaxed) == ver) [[likely]]
	return std::pair(v,ver);
      spinDelay(std::chrono::nanoseconds(delay)); 
      delay = std::min(2 * delay, 5000);
    }
  }

  PARLAY_INLINE
  std::optional<std::pair<V,tag>> try_ll() {
    vtype ver1 = version.load(std::memory_order_acquire);
    V v = val;
    std::atomic_thread_fence(std::memory_order_acquire);
    vtype ver2 = version.load(std::memory_order_relaxed);
    if ((ver1 & 1) == 0 && ver1 == ver2) [[likely]]
      return std::pair(v, ver2);
    if ((ver1 & 1) == 0) return {};
    while (ver2 == ver1) 
      ver2 = version.load(std::memory_order_acquire);
    v = val;
    std::atomic_thread_fence(std::memory_order_acquire);
    vtype ver3 = version.load(std::memory_order_relaxed);
    if (ver3 == ver1 + 1) return std::pair(v, ver3);
    return {};
  }

  PARLAY_INLINE
  bool lv(tag tg) {
    return version.load() == tg;
  }

  // If successful, linearizes when taking the lock
  // If failure on comparison, then linearize on load
  // If failure on CAS, then linearize on CAS
  PARLAY_INLINE
  bool sc(tag expected_tag, const V& v) {
    vtype ver = version.load();
    if (ver == expected_tag &&
        version.compare_exchange_strong(ver, ver + 1, std::memory_order_relaxed)) {
      std::atomic_thread_fence(std::memory_order_release);
      val = v;
      version.store(ver + 2, std::memory_order_release);
      return true;
    } else return false;
  }

  PARLAY_INLINE V load() { return ll().first; }

  PARLAY_INLINE bool cas(const V& expected, const V& desired) {
    auto r = try_ll();
    if (!r) return false;
    auto [current, tag] = *r;
    if (!KeyEqual{}(current, expected)) return false;
    if (KeyEqual{}(expected, desired)) return true;
    return sc(tag, desired);
  }

};

}  // namespace parlay
#endif  // PARLAYATOMIC_H_
