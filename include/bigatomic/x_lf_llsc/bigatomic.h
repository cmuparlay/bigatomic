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
#include <common/free_list.h>

//#include "hazard_ptr.h"

namespace parlay {

  template<typename T, typename Equal = std::equal_to<>>
  struct big_atomic;
  using tag = int64_t;

  namespace internal {

template<typename T, typename Equal>
class NodeManager;

template<typename T, typename Equal>
extern inline NodeManager<T, Equal>& node_manager();

template<typename T, typename Equal>
struct big_atomic_node {
  union {
    big_atomic_node* next_;     // Intrusive link for free list
    T value;
  };
  tag version;

  const unsigned owner{node_manager<T, Equal>().my_thread_id()};  // ID of the owning thread
  std::atomic<bool> is_installed{false};
  bool is_protected{false};
  bool was_installed{false};

  big_atomic_node() noexcept : next_{nullptr} {}
  ~big_atomic_node() = default;
};

template<typename T, typename Equal>
class NodeManager {

  using big_atomic_type = big_atomic<T, Equal>;
  using node_type = big_atomic_node<T, Equal>;

  // The minimum number of backup nodes to pre-allocate for each thread.  If a thread ever manages to run out, it will
  // allocate another set of this many on top of whatever it already had.  This can only happen if the user spawns
  // more threads, so there should be no allocations after the warmup run if the number of threads stays the same.
  constexpr static std::size_t slab_size = 4096; //1024;

  struct NodeMemorySlab {
    std::array<node_type, slab_size> nodes;
    NodeMemorySlab* next_;
  };

  // The slots are linked together to form a linked list so that threads can scan
  // for the set of currently protected pointers.
  //
  struct alignas(CACHE_LINE_ALIGNMENT) AnnouncementSlot {
    explicit AnnouncementSlot(unsigned thread_id_, bool in_use_) : thread_id(thread_id_), in_use(in_use_) {}

    // Announced node and big_atomic
    std::atomic<node_type*> protected_node{nullptr};

    // Link together all existing slots into a big global linked list
    std::atomic<AnnouncementSlot*> next{nullptr};

    // Private linked lists of free (available) nodes
    IntrusiveFreeList<node_type> free_nodes{};

    // Private slab allocated nodes.  Slabs are connected in a linked list
    NodeMemorySlab* my_nodes{nullptr};

    // Unique ID for the owner of this slot (not guaranteed to be in order)
    unsigned thread_id;

    // True if this hazard pointer slot is owned by a thread.
    std::atomic<bool> in_use;
  };

  // Find an available hazard slot, or allocate a new one if none available.
  AnnouncementSlot* get_slot() {
    auto current = announcement_list;
    while (true) {
      if (!current->in_use.load() && !current->in_use.exchange(true)) {
        return current;
      }
      if (current->next.load() == nullptr) {
        // thread_ids are not guaranteed to be installed in the linked list in order
        auto my_slot = new AnnouncementSlot{num_threads.fetch_add(1), true};
        AnnouncementSlot* next = nullptr;
        while (!current->next.compare_exchange_weak(next, my_slot)) {
          current = next;
          next = nullptr;
        }
        return my_slot;
      } else {
        current = current->next.load();
      }
    }
  }

  // Give a slot back to the world so another thread can re-use it
  void relinquish_slot(AnnouncementSlot* slot) { slot->in_use.store(false); }

  // A AnnouncementSlotOwner owns exactly one AnnouncementSlot entry in the global linked list
  // of AnnouncementSlots.  On creation, it acquires a free slot from the list, or appends
  // a new slot if all of them are in use.  On destruction, it makes the slot available
  // for another thread to pick up.
  struct AnnouncementSlotOwner {
    explicit AnnouncementSlotOwner(NodeManager<T, Equal>& list_) : list(list_), my_slot(list.get_slot()) {}

    ~AnnouncementSlotOwner() { list.relinquish_slot(my_slot); }

  private:
    NodeManager<T, Equal>& list;

  public:
    AnnouncementSlot* const my_slot;
  };

public:
  // Pre-populate the slot list with P slots, one for each hardware thread
  NodeManager() : announcement_list(new AnnouncementSlot{0, false}) {
    auto current = announcement_list;
    for (unsigned i = 1; i < std::thread::hardware_concurrency(); i++) {
      current->next = new AnnouncementSlot{i, false};
      current = current->next;
    }
    num_threads.store(std::thread::hardware_concurrency());
  }

  NodeManager(const NodeManager&) = delete;
  NodeManager& operator=(const NodeManager&) = delete;

  ~NodeManager() = delete;

  [[nodiscard]] unsigned my_thread_id() const noexcept { return local_data.my_slot->thread_id; }

  PARLAY_INLINE
  tag protect_node(const std::atomic<tag>& src) noexcept {
    auto& announcement_slot = local_data.my_slot->protected_node;
    auto result = src.load(std::memory_order_acquire);

    while (true) {
      if (result < 0) return result;
      auto rp = (node_type*) result;
      PARLAY_PREFETCH(rp, 0, 0);
      announcement_slot.exchange(rp, std::memory_order_seq_cst);

      auto current_value = src.load(std::memory_order_acquire);
      if (current_value == result) [[likely]] {
        return result;
      } else {
        result = current_value;
      }
    }
  }

  PARLAY_INLINE node_type* get_free_node() {
    auto& slot = *local_data.my_slot;
    if (slot.free_nodes.empty()) [[unlikely]] {
      reclaim_free_nodes(slot);
      assert(!slot.free_nodes.empty());
    }
    auto node = slot.free_nodes.pop();
    return node;
  }

  PARLAY_INLINE
  void free_node(node_type* node) {
    auto& slot = *local_data.my_slot;
    slot.free_nodes.push(node);
  }

private:
  PARLAY_NOINLINE PARLAY_COLD void reclaim_free_nodes(AnnouncementSlot& slot) {

    if (slot.my_nodes != nullptr) {

      for_each_node([&](auto node) {
        node->was_installed = node->is_installed.load(std::memory_order_relaxed);
      });

      for_each_slot([&](auto& announcement) {
        node_type* a = announcement.protected_node.load();
        if (a != nullptr && a->owner == slot.thread_id) {
          a->is_protected = true;
        }
      });

      for_each_node([&](auto node) {
        if (!node->was_installed && !node->is_protected) {
          slot.free_nodes.push(node);
        }
        node->is_protected = false;
      });
    }

    // Nothing available even after reclaiming -- need to allocate more nodes.  This should only happen either
    // (a) the first time it is ever used, or (b) when new threads have been spawned since the last use.
    if (slot.free_nodes.empty()) [[unlikely]] {
      assert(slot.my_nodes == nullptr);   // True as long as we have < 300 threads.  Should remove this for a proper release version.
      grow_nodes(slot);
    }
  }

  // Allocate another slab of nodes. This should only occur when the current thread runs out of
  // nodes, meaning there are more threads than the current number can handle.
  void grow_nodes(AnnouncementSlot& slot) {
    auto new_slab = new NodeMemorySlab{{}, slot.my_nodes};
    slot.my_nodes = new_slab;
    for (auto& node : new_slab->nodes) {
      slot.free_nodes.push(&node);
    }
  }

  template<typename F>
  void for_each_slot(F&& f) noexcept(std::is_nothrow_invocable_v<F&, AnnouncementSlot&>) {
    for (auto current = announcement_list; current != nullptr; current = current->next.load()) {
      f(*current);
    }
  }

  template<typename F>
  void for_each_node(F&& f) noexcept(std::is_nothrow_invocable_v<F&, node_type*>) {
    for (auto slab = local_data.my_slot->my_nodes; slab != nullptr; slab = slab->next_) {
      for (auto& node : slab->nodes) {
        f(&node);
      }
    }
  }

  AnnouncementSlot* const announcement_list;
  std::atomic<unsigned> num_threads{0};

  static inline const thread_local AnnouncementSlotOwner local_data{node_manager<T, Equal>()};
};

template<typename T, typename Equal>
NodeManager<T, Equal>& node_manager() {
  alignas(NodeManager<T, Equal>) static char buffer[sizeof(NodeManager<T, Equal>)];
  static auto* list = new (&buffer) NodeManager<T, Equal>{};
  return *list;
}

}
  
template<typename T, typename Equal> // = std::equal_to<>>
struct big_atomic {
  // T must be trivially copyable, but it doesn't have to be trivially default constructible (or
  // even default constructible at all) since we only make copies from what the user gives us

  // Guy: had to comment these checks out to work with parlayhash since it uses std::array
  //static_assert(std::is_trivially_copyable_v<T>);
  //static_assert(std::is_invocable_r_v<bool, Equal, T&&, T&&>);

private:

    //static_assert(alignof(big_atomic_node) >= 2, "Need a spare bit to mark pointers");

public:

  using node_type = internal::big_atomic_node<T, Equal>;
  using node_manager_type = internal::NodeManager<T, Equal>;

  friend node_type;
  friend node_manager_type;
  
  using value_type = T;
  using version_type = long;

  big_atomic() : header(std::numeric_limits<long>::min()) {
    // force correct static initialization order
    node_manager();
    store_sequential(T{});
  }

 big_atomic(const T& initial) : header(std::numeric_limits<long>::min()) {
    node_manager();
    store_sequential(initial);
  }

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
        //auto hazptr = hazptr_holder{};
        tag slot = node_manager().protect_node(header);
        if (is_pointer(slot)) {
          auto* p = (node_type*) slot;
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
      return expected_tag == hdr || ((node_type*) expected_tag)->version == hdr;
    } else [[likely]] {return hdr == expected_tag;}
  }

  PARLAY_INLINE bool sc(tag expected_tag, const T& v) {
    //auto hazptr = hazptr_holder{};
    tag old_hdr = header.load(); //std::memory_order_relaxed);
    tag new_tag;
    if (is_version(expected_tag)) [[likely]] {
      if (old_hdr != expected_tag) return false;
      new_tag = old_hdr + 1;
    } else { // expected tag is a pointer
      auto expected_ptr = (node_type*) expected_tag;
      if (is_pointer(old_hdr)) { // also a pointer
        if (expected_tag != old_hdr) return false;
        new_tag = expected_ptr->version + 1;
      } else { // now a tag
        if (old_hdr != expected_ptr->version) return false;
        new_tag = old_hdr + 1;
      }
    }
    node_type* new_ptr = make_node(v, new_tag);
    if (is_pointer(old_hdr)) // delay to reduce contention
      for (volatile int i = 0; i < 2000; i++);
    //hazptr.protect_direct(new_ptr);

    // try to install new value (linerization point if succeeds)
    tag tmp_hdr = header.load(std::memory_order_relaxed);
    if (tmp_hdr != old_hdr ||
        !header.compare_exchange_strong(tmp_hdr, (tag) new_ptr)) [[unlikely]]
      // if failed because current value is a seq number, and old_hdr
      // was a pointer with same seq num, try again             
      if (!is_version(tmp_hdr) || (tmp_hdr != new_tag - 1) ||
          !header.compare_exchange_strong(tmp_hdr, (tag) new_ptr)) {
        node_manager().free_node(new_ptr);
        return false;
      }

    if (is_pointer(tmp_hdr)) // was a pointer, do not install cache
      ((node_type*) old_hdr)->is_installed.store(false, std::memory_order_relaxed);
    //retire((node_type*) old_hdr);
    else { // try to install the cached value
      store_sequential(v);
      while (header.load(std::memory_order_relaxed) != (tag) new_ptr ||
             !header.compare_exchange_weak((tag&) new_ptr, new_tag)) {
        new_ptr = (node_type*) node_manager().protect_node(header);
        store_sequential(new_ptr->value);
        new_tag = new_ptr->version;
      }
      new_ptr->is_installed.store(false, std::memory_order_relaxed);
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
    //if (is_pointer(hdr)) {
    //  node_manager().free_node((node_type*) hdr);
    //}
  }

 private:

  node_type* make_node(const T& desired, const tag new_tag) noexcept {
    auto new_node = node_manager().get_free_node();
    new_node->value = desired;
    new_node->version = new_tag;
    new_node->is_installed.store(true, std::memory_order_relaxed);
    return new_node; //marked_node_ptr_type::create_ptr(new_node);
  }

  PARLAY_INLINE static bool is_version(long tag) {
    return tag < 0;
  }

  PARLAY_INLINE static bool is_pointer(long tag) {
    return !is_version(tag);
  }

  static internal::NodeManager<T, Equal>& node_manager() { return internal::node_manager<T, Equal>(); }

  // Either pointer or sequence number
  std::atomic<tag> header;
  alignas(8) char fast_value[sizeof(T)]; 
};


}  // namespace parlay
