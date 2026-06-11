// An implementation of big_atomic using a shared lock pool.
//
//  Supports:
//  - Blocking loads
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
#include <common/utility.h>

namespace parlay {

template<typename V, class KeyEqual = std::equal_to<V>>
struct big_atomic {
  std::mutex my_mutex;
  V val;

  /* implicit */ big_atomic(const V& v) : val(v) {}  // NOLINT(google-explicit-constructor)
  big_atomic() = default;

  V load() {
    V result;
    // some testing went into optimizing the delays
    int delay = 5000;
    while (true) {
      if (get_locks().try_lock((long)this, [&] {
            result = val;
            return true;
          }))
        return result;
      delay = std::min(2 * delay, 20000);
      spinDelay(std::chrono::nanoseconds(delay)); 
    }
  }

  void store(const V& v) {
    int delay = 5000;
    while (true) {
      if (get_locks().try_lock((long)this, [&] {
            val = v;
            return true;
          }))
        return;
      delay = std::min(2 * delay, 20000);
      spinDelay(std::chrono::nanoseconds(delay)); 
    }
  }

  bool cas(const V& expected_v, const V& v) {
    bool result = true;
    int delay = 5000;
    while (true) {
      if (get_locks().try_lock((long)this, [&] {
            if (!(val == expected_v))
              result = false;
            else
              val = v;
            return true;
          }))
        return result;
      delay = std::min(2 * delay, 20000);
      spinDelay(std::chrono::nanoseconds(delay)); 
    }
  }

  // NOTE: Versions based on std:lock are slower
  // V load() {
  //   std::unique_lock<std::mutex> lock(my_mutex);
  //   return val;
  // }

  // bool cas(const V& expected_v, const V& v) {
  //   std::unique_lock<std::mutex> lock(my_mutex);
  //   if (!(val == expected_v))
  //     return false;
  //   val = v;
  //   return true;
  // }
};

}  // namespace parlay
#endif  // PARLAYATOMIC_H_
