// An implementation of big_atomic via std::atomic.  std::atomic uses GNU libatomic
// for multi-word atomics on GCC, which internally uses a lock pool.
//

#ifndef PARLAYATOMIC_H_
#define PARLAYATOMIC_H_

#include <atomic>
#include <functional>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

namespace parlay {
  template <typename V,
	    class KeyEqual = std::equal_to<V>>
  struct big_atomic {
    std::atomic<V> val;
    /* implicit */ big_atomic(const V& v) : val(v) {}   // NOLINT(google-explicit-constructor)
    big_atomic() : val(V{}) {}
    ~big_atomic() = default;
    V load() { return val.load();}
    void store(const V& new_v) { val.store(new_v);}
    bool cas(V expected_v, const V& new_v) {
      return val.compare_exchange_strong(expected_v, new_v);
    }
  };
} // namespace parlay

#endif //PARLAYATOMIC_H_
