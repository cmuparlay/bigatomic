// An implementation of big_atomic using a SeqLock & indirect backup values with Hazard-Pointer reclamation.
//
//  Supports:
//  - Wait-free load
//  - Wait-free store
//  - Wait-free CAS (with spurious failures in the presence of stores)
//
// Uses up to O(n + p^2) extra space.  Uses the Parlay allocator.
//

#ifndef PARLAYATOMIC_H_
#define PARLAYATOMIC_H_

#include <atomic>
#include <functional>
#include <limits>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include <common/hazard.h>

namespace parlay {

template<typename V, class KeyEqual = std::equal_to<V>>
struct big_atomic {

 private:
  using vtype = int64_t;

  std::atomic<V*> ptr;
  std::atomic<vtype> version;
  V cache;

  // a bit in the pointer is used to mark whether the cache is valid or not
  PARLAY_INLINE
  static V* mark_invalid(V* ptr) { return (V*)(((size_t)ptr) | 1ul); }
    PARLAY_INLINE
  static V* remove_mark(V* ptr) { return (V*)(((size_t)ptr) & ~1ul); }
    PARLAY_INLINE
  static bool is_valid(V* ptr) { return ((size_t)ptr & 1) == 0; }

  bool try_install(V* p_old, V* p_new) {
    V* p_tmp = p_old;
    // try to swap in p_new for p_old
    if (p_old == ptr.load() && ptr.compare_exchange_strong(p_old, p_new)) return true;
    // if failed the first time, and this was because p_old was
    // validated (i.e. with the same value) then we need to try
    // again.
    V* x = ptr.load();
    return (remove_mark(p_tmp) == x && ptr.compare_exchange_strong(x, p_new));
  }

  void try_update_cache(V v, V* p, vtype ver) {
    // try to lock the cache
    if (!(ver & 1) && ver == version.load() && version.compare_exchange_strong(ver, ver + 1)) {
      // when locked load the cache
      std::atomic_thread_fence(std::memory_order_release);
      cache = v;
      // unlock the cache
      version.store(ver + 2, std::memory_order_release);
      // try to validate the cache if p has not changed
      ptr.compare_exchange_strong(p, remove_mark(p));
    }
  }

  // try to update the atomic with new_v.  old_p is the current
  // pointer (consistent with the old value), and ver the current
  // version.
  bool try_update(V new_v, V* old_p, vtype ver) {
    V* new_p = mark_invalid(hazard::protect(hazard::New<V>(new_v), 1));
    if (try_install(old_p, new_p)) {
      hazard::Retire(remove_mark(old_p));
      try_update_cache(new_v, new_p, ver);
      return true;
    } else {
      hazard::Delete(remove_mark(new_p));
      return false;
    }
  }

 public:
  big_atomic(const V& v) : ptr(hazard::New<V>()),
                           version(std::numeric_limits<int64_t>::min()),
                           cache(v) {}
  big_atomic() : ptr(hazard::New<V>()),
                 version(std::numeric_limits<int64_t>::min()) {}
  ~big_atomic() { hazard::Delete(remove_mark(ptr.load())); }

  V load_() {
    vtype ver = version.load(std::memory_order_acquire);
    V v = cache;
    V* p = ptr.load();
    std::atomic_thread_fence(std::memory_order_acquire);
    // check if value in the cache is valid
    if (is_valid(p) && version.load(std::memory_order_relaxed) == ver) return v;
    return *remove_mark(hazard::load_protected(ptr, 0));
  }

  void store(const V& new_v) { return try_update(new_v, ptr.load(), version.load()); }

  bool cas_(const V& expected_v, const V& new_v) {
    vtype ver = version.load(std::memory_order_acquire);
    __builtin_prefetch(ptr.load());
    V old_v = cache;
    return hazard::with_protection(&ptr, 0, [&](V* old_p) {
      std::atomic_thread_fence(std::memory_order_acquire);
      // check if value in the cache is valid
      if (!(is_valid(old_p) && version.load(std::memory_order_relaxed) == ver))
        old_v = *remove_mark(old_p);
      if (!(old_v == expected_v)) return false;
      if (expected_v == new_v) return true;
      return try_update(new_v, old_p, ver);
    });
  }

  using tag = int64_t; 
  
  void store_sequential(const V& desired) {
    cache = desired;
  }

  PARLAY_INLINE std::pair<V,tag> ll() {
    vtype ver = version.load(std::memory_order_acquire);
    V v = cache;
    V* p = ptr.load();
    std::atomic_thread_fence(std::memory_order_acquire);
    // check if value in the cache is valid
    if (is_valid(p) && version.load(std::memory_order_relaxed) == ver) [[likely]] 
        return std::pair(v, ver);
    p = hazard::load_protected(ptr, 0); 
    return std::pair(*remove_mark(p), (int64_t) p);
  }

  PARLAY_INLINE bool lv(tag expected_tag) {
    V* p = ptr.load();
    if (expected_tag < 0) [[likely]] { // i.e. a version
      auto ver = version.load(std::memory_order_acquire);
      return (is_valid(p) && ver == expected_tag);
    } else
      return (p == (V*) expected_tag);
  }

  PARLAY_INLINE bool sc(tag expected_tag, const V& new_v) {
    vtype ver;
    V* p;
    if (expected_tag < 0) [[likely]] { // i.e. a version
      p = hazard::load_protected(ptr, 0);
      ver = version.load(std::memory_order_acquire);
      if (!is_valid(p) || ver != expected_tag) [[unlikely]] return false;
    } else {
      p = (V*) expected_tag;
      ver = version.load(std::memory_order_acquire);
    }
    return try_update(new_v, p, ver);
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
