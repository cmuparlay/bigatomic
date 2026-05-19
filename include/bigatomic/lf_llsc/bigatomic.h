#pragma once
// A load reads the header, reads the cache, and reads the header again.   If both reads of the header are the same sequence number, return the value from the cache.   If they are both different sequence numbers, then try again, and if either was a pointer, then protected read the value through one of the pointers and return it.   If a ll, return the header as the tag (if a pointer, whichever was used for the value).

// An sc given a tag:
// protected read the header and:
// If the tag was a sequence number, 
//    if the header is not equal to the tag, the sc fails and returns false
//    else copy the new value and tag+1 (new sequence number) to a new object and try to cas this in over the tag (sequence number) in the header
//        if the cas fails, then the sc fails, deletes the object and returns false
//        else copy the value into the cache
//          try to cas tag+1 (new sequence number) into the header
//          if succeeds then retire object and return true
//          else try to install cache again: i.e., 
//            protected read the new header, get value and seq num from it, install value in cache,  and try to cas in seq num 
//            repeat until succeeds (only the sc that swapped out a sequence number will swap one back in so each try will only see a pointer)
// else tag was a pointer
//    if the header is a pointer and different, the sc fails and returns false
//    if the header is a sequence number and does not match the sequence number in the object pointed to by the tag, the sc fails and returns false 
//    otherwise
//       new seq number is 1 + header if header is a sequence number or 1 + object seq number if a pointer
//       copy new value and new seq number to new object and try to cas it in over the tag
//       if the cas fails, the sc fails, deletes the object, and returns false
//       else  if header was a pointer, retire old object 
//          return true

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <atomic>
#include <functional>

#include <parlay/alloc.h>

#include <common/utility.h>

#include "hazard_ptr.h"

namespace parlay {

template<typename T, typename Equal = std::equal_to<>>
struct big_atomic {
  // T must be trivially copyable, but it doesn't have to be trivially default constructible (or
  // even default constructible at all) since we only make copies from what the user gives us

  // Guy: had to comment these checks out to work with parlayhash since it uses std::array
  //static_assert(std::is_trivially_copyable_v<T>);
  //static_assert(std::is_invocable_r_v<bool, Equal, T&&, T&&>);

 private:
  struct indirect_holder {
    explicit indirect_holder(const T& value, long version) : value(value), version(version) { }
    T value;
    long version;
    indirect_holder* next_;    // Intrusive link for hazard pointers
    indirect_holder* get_next() { return next_; }
    void set_next(indirect_holder* next) { next_ = next; }
    void destroy() {  allocator::destroy(this); }
  };

  static_assert(alignof(indirect_holder) >= 2, "Need a spare bit to mark pointers");

 public:

  using value_type = T;
  using version_type = long;

  using allocator = type_allocator<indirect_holder>;

  big_atomic() : header(std::numeric_limits<long>::min()) {
    static_assert(std::is_default_constructible_v<T>);
    // force correct static initialization order
    allocator::init();
    hazptr_instance();
    store_sequential(T{});
  }

  big_atomic(const T& initial) : header(std::numeric_limits<long>::min()) {
    allocator::init();
    hazptr_instance();
    store_sequential(initial);
  }

  // Either sequence number or pointer to indirect slot
  using tag = long; 
  
  void store_sequential(const T& desired) {
    atomic_store_per_byte_memcpy(&fast_value, &desired, sizeof(T)); }

  PARLAY_INLINE void load_sequential(char* buffer) {
     atomic_load_per_byte_memcpy(buffer, &fast_value, sizeof(T));
  }

  PARLAY_INLINE std::pair<T, tag> ll() {
    while (true) {
      alignas(T) char buffer[sizeof(T)];
      tag fst_hdr = header.load(std::memory_order_acquire);
      load_sequential(buffer);
      tag snd_hdr = header.load(std::memory_order_relaxed);
      if (is_pointer(fst_hdr) || is_pointer(snd_hdr)) {
        // at least one was pointer
        // protected load of slot
        auto hazptr = hazptr_holder{};
        tag slot = hazptr.protect(header);
        if (is_pointer(slot)) {
          auto* p = (indirect_holder*) slot;
          return {p->value, slot};
          // still a pointer
        }
      } else if (fst_hdr == snd_hdr) [[likely]] {
        // matching seqnums
        return {bits_to_object<T>(buffer), fst_hdr};
      }
      // otherwise try again
    }
    assert(false && "Impossible");
  }

  PARLAY_INLINE T load() {
    return ll().first;
  }

  PARLAY_INLINE bool lv(tag expected_tag) {
    tag hdr = header.load(std::memory_order_acquire);
    if (is_pointer(expected_tag)) { 
      return expected_tag == hdr || ((indirect_holder*) expected_tag)->version == hdr;
    } else [[likely]] {return hdr == expected_tag;}
  }

  // An sc given a tag:
  // protected read the header and:
  // If the tag was a sequence number, 
  //    if the header is not equal to the tag, the sc fails and returns false
  //    else copy the new value and tag+1 (new sequence number) to a new object and try to cas this in over the tag (sequence number) in the header
  //        if the cas fails, then the sc fails, deletes the object and returns false
  //        else copy the value into the cache
  //          try to cas tag+1 (new sequence number) into the header
  //          if succeeds then retire object and return true
  //          else try to install cache again: i.e., 
  //            protected read the new header, get value and seq num from it, install value in cache,  and try to cas in seq num 
  //            repeat until succeeds (only the sc that swapped out a sequence number will swap one back in so each try will only see a pointer)
  // else tag was a pointer
  //    if the header is a pointer and different, the sc fails and returns false
  //    if the header is a sequence number and does not match the sequence number in the object pointed to by the tag, the sc fails and returns false 
  //    otherwise
  //       new seq number is 1 + header if header is a sequence number or 1 + object seq number if a pointer
  //       copy new value and new seq number to new object and try to cas it in over the tag
  //       if the cas fails, the sc fails, deletes the object, and returns false
  //       else  if header was a pointer, retire old object 
  //          return true

  PARLAY_INLINE bool sc_(tag expected_tag, const T& desired) {
    auto hazptr = hazptr_holder{};
    tag hdr = hazptr.protect(header);
    if (is_version(expected_tag)) [[likely]] { // i.e. a version number
      // load current tag
      if (hdr != expected_tag)
        // store in meantime, LL fails
        return false;
      // incremented version
      tag new_tag = expected_tag + 1;
      // Create new indirect slot to try to install
      auto* new_hdr = allocator::create(desired, new_tag);
      hazptr.protect_direct(new_hdr);
      // Try to install new slot
      if (header.load(std::memory_order_relaxed) != hdr ||
          !header.compare_exchange_strong(hdr, (tag) new_hdr)) {
        // Failed to install new header, free it
        // No other thread maintains a reference to it
        //hazptr.unprotect_direct(new_hdr);
        allocator::destroy(new_hdr);
        return false;
      }
      // Write desired value to cache
      store_sequential(desired);
      // hazard pointer slots
      while (header.load(std::memory_order_relaxed) != (tag) new_hdr ||
          !header.compare_exchange_weak((tag&) new_hdr, new_tag)) {
        // Failed to install new tag
        // Load current header and try to cache that instead
        // TODO is this correct?
        new_hdr = (indirect_holder*) hazptr.protect(header);
        // assert(is_pointer(new_hdr) && "Got seqnum during sc");
        store_sequential(new_hdr->value);
        new_tag = new_hdr->version;
      }
      retire(new_hdr);
      return true;
    } else {
      // expected_tag is pointer
      if (is_pointer(hdr)) {
        // current tag also pointer
        if (expected_tag != hdr)
          // store in meantime
          return false;
        auto* hdr_ptr = (indirect_holder*) hdr;
        auto curr_version = hdr_ptr->version;
        auto* new_hdr = allocator::create(desired, 1 + curr_version);
        if (header.load(std::memory_order_relaxed) != hdr ||
            !header.compare_exchange_strong(hdr, (tag) new_hdr)) {
          // failed to install new slot
          allocator::destroy(new_hdr);
          return false;
        }
        // successfully installed slot, retire old one
        retire(hdr_ptr);
        return true;
      } else {
        // current tag is seqnum
        if (hdr != ((indirect_holder*) expected_tag)->version)
          // different sequnum than in object
          return false;
        auto* new_hdr = allocator::create(desired, 1 + hdr);
        if (header.load(std::memory_order_relaxed) == hdr &&
            header.compare_exchange_strong(hdr, (tag) new_hdr)) {
          // Write desired value to cache
          store_sequential(desired);
          auto new_tag = new_hdr->version;
          // hazard pointer slots
          while (header.load(std::memory_order_relaxed) != (tag) new_hdr ||
                 !header.compare_exchange_weak((tag&) new_hdr, new_tag)) {
            // Failed to install new tag
            // Load current header and try to cache that instead
            // TODO is this correct?
            new_hdr = (indirect_holder*) hazptr.protect(header);
            // assert(is_pointer(new_hdr) && "Got seqnum during sc");
            store_sequential(new_hdr->value);
            new_tag = new_hdr->version;
          }
          retire(new_hdr);
          return true;
        }
        allocator::destroy(new_hdr);
        return false;
      }
    }
  }

  PARLAY_INLINE bool sc(tag expected_tag, const T& v) {
    auto hazptr = hazptr_holder{};
    tag old_hdr = header.load(); //std::memory_order_relaxed);
    tag new_tag;
    if (is_version(expected_tag)) [[likely]] {
      if (old_hdr != expected_tag) return false;
      new_tag = old_hdr + 1;
    } else { // expected tag is a pointer
      auto expected_ptr = (indirect_holder*) expected_tag;
      if (is_pointer(old_hdr)) { // also a pointer
        if (expected_tag != old_hdr) return false;
        new_tag = expected_ptr->version + 1;
      } else { // now a tag
        if (old_hdr != expected_ptr->version) return false;
        new_tag = old_hdr + 1;
      }
    }
    indirect_holder* new_ptr = allocator::create(v, new_tag);
    if (is_pointer(old_hdr)) // delay to reduce contention
      for (volatile int i = 0; i < 1000; i++);
    hazptr.protect_direct(new_ptr);

    // try to install new value (linerization point if succeeds)
    tag tmp_hdr = header.load(std::memory_order_relaxed);
    if (tmp_hdr != old_hdr ||
        !header.compare_exchange_strong(tmp_hdr, (tag) new_ptr)) [[unlikely]]
      // if failed because current value is a seq number, and old_hdr
      // was a pointer with same seq num, try again             
      if (!is_version(tmp_hdr) || (tmp_hdr != new_tag - 1) ||
          !header.compare_exchange_strong(tmp_hdr, (tag) new_ptr)) {
        allocator::destroy(new_ptr); // installation failed
        return false;
      }
    
    if (is_pointer(tmp_hdr)) // was a pointer, do not install cache
      retire((indirect_holder*) old_hdr);
    else { // try to install the cached value
      store_sequential(v);
      while (header.load(std::memory_order_relaxed) != (tag) new_ptr ||
             !header.compare_exchange_weak((tag&) new_ptr, new_tag)) {
        //for (volatile int i =0; i < 1000; i++); // delay to reduce contention
        new_ptr = (indirect_holder*) hazptr.protect(header);
        store_sequential(new_ptr->value);
        new_tag = new_ptr->version;
      }
      retire(new_ptr);
    }
    return true;
  }

  PARLAY_INLINE bool cas(const T& expected, const T& desired) {
    auto [current, tag] = ll();
    if (!Equal{}(current, expected)) return false;
    if (Equal{}(expected, desired)) return true;
    return sc(tag, desired);
  }

  ~big_atomic() {
    auto hdr = header.load(std::memory_order_relaxed);
    if (is_pointer(hdr)) {
      allocator::destroy((indirect_holder*) hdr);
    }
  }

 private:

  struct hazptr_holder {
    PARLAY_INLINE
    tag protect(const std::atomic<tag>& src) const {
      return hazptr_instance().template protect<indirect_holder>(src);
    }
    PARLAY_INLINE
    void protect_direct(indirect_holder* src) const {
      hazptr_instance().protect_direct(src);
    }
    void unprotect_direct(indirect_holder* src) const {
      hazptr_instance().unprotect_direct(src);
    }
  };

  PARLAY_INLINE
  static void retire(indirect_holder* p) {
    if (p) {
      hazptr_instance().retire(p);
    }
  }

  PARLAY_INLINE
  static HazardPointers<indirect_holder>& hazptr_instance() {
    return get_hazard_list<indirect_holder>();
  }

  PARLAY_INLINE static bool is_version(long tag) {
    return tag < 0;
  }

  PARLAY_INLINE static bool is_pointer(long tag) {
    return !is_version(tag);
  }

  // Either pointer or sequence number
  std::atomic<tag> header;
  alignas(8) char fast_value[sizeof(T)]; 
};


}  // namespace parlay
