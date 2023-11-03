// An implementation of big_atomic using SeqLocks and short-lived indirect backup values.
//
//  Simple:
//  - Wait-free load
//  - Wait-free store
//  - Wait-free CAS (with spurious failures in the presence of stores)
//
// Guaranteed at most O(P^2) extra space, and no heap allocations after warmup.
//
// Note: This version currently has an unfortunate extra requirement:  Any memory that is used for a big_atomic
// should not be reclaimed and re-used, because a slow thread might try to write to a big_atomic after it gets
// freed and possibly re-used.  Fixing this would be good, but probably requires making the destructor of
// big_atomic blocking to avoid this.
//

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <atomic>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <parlay/portability.h>

#include <common/marked_ptr.h>
#include <common/utility.h>

namespace parlay {

template<typename T, typename Equal = std::equal_to<>>
struct big_atomic;

namespace internal {

template<typename T, typename Equal>
class NodeManager;

template<typename T, typename Equal>
extern inline NodeManager<T, Equal>& node_manager();

template<typename T, typename Equal>
struct big_atomic_node {
  union {
    big_atomic_node* next_;  // Intrusive link for free list
    T value;
  };
  std::atomic<big_atomic<T, Equal>*> src_;                        // Ptr to big_atomic most recently installed in
  const unsigned owner{node_manager<T, Equal>().my_thread_id()};  // ID of the owning thread
  std::atomic<bool> owns_refcount{false};
  std::atomic<bool> pending_delayed_operation{false};
  bool was_installed{false};
  bool is_protected{false};

  big_atomic_node() noexcept : next_() {}
  ~big_atomic_node() = default;
};

template<typename T, typename Equal>
class NodeManager {

  using big_atomic_type = big_atomic<T, Equal>;
  using node_type = big_atomic_node<T, Equal>;
  using marked_node_ptr_type = marked_ptr<node_type>;

  // The minimum number of backup nodes to pre-allocate for each thread.  If a thread ever manages to run out, it will
  // allocate another set of this many on top of whatever it already had.  This can only happen if the user spawns
  // more threads, so there should be no allocations after the warmup run if the number of threads stays the same.
  constexpr static std::size_t slab_size = 1024;

  struct NodeMemorySlab {
    std::array<node_type, slab_size> nodes;
    NodeMemorySlab* next_;
  };

  // A basic single-thread private linked list.  The values are required to expose a public next_ field.
  class IntrusiveFreeList {
   public:
    IntrusiveFreeList() : head(nullptr) {}

    void push(node_type* node) noexcept { node->next_ = std::exchange(head, node); }
    node_type* pop() noexcept {
      assert(!empty());
      return std::exchange(head, head->next_);
    }
    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    template<typename F>
    void for_each(F&& f) {
      for (auto current = head; current != nullptr; current = current->next_) {
        f(current);
      }
    }

   private:
    node_type* head;
  };

  // The slots are linked together to form a linked list so that threads can scan
  // for the set of currently protected pointers.
  //
  struct alignas(CACHE_LINE_ALIGNMENT) AnnouncementSlot {
    explicit AnnouncementSlot(unsigned thread_id_, bool in_use_) : thread_id(thread_id_), in_use(in_use_) {}

    // Announced node and big_atomic
    std::atomic<node_type*> protected_node{nullptr};
    std::atomic<big_atomic_type*> protected_src{nullptr};

    // Link together all existing slots into a big global linked list
    std::atomic<AnnouncementSlot*> next{nullptr};

    // Private linked lists of free (available) nodes
    IntrusiveFreeList free_nodes{};

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
        // thread_ids are not guaratneed to be installed in the linked list in order
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

  void protect_src(big_atomic_type* const ptr) noexcept {
    local_data.my_slot->protected_src.store(ptr, std::memory_order_seq_cst);
  }

  marked_node_ptr_type protect_node(const std::atomic<marked_node_ptr_type>& src) noexcept {
    auto& announcement_slot = local_data.my_slot->protected_node;
    auto result = src.load(std::memory_order_acquire);

    while (true) {
      if (result.get_ptr() == nullptr) return result;
      PARLAY_PREFETCH(result.get_ptr(), 0, 0);
      announcement_slot.exchange(result.unmark(), std::memory_order_seq_cst);

      auto current_value = src.load(std::memory_order_acquire);
      if (current_value.get_ptr() == result.get_ptr()) [[likely]] {
        return result;
      } else {
        result = current_value;
      }
    }
  }

  // Announce a given node.  The caller needs to guarantee that the node has not
  // been made eligible for reclamation before this point.
  void announce(marked_node_ptr_type p) noexcept {
    assert(p.get_ptr() != nullptr);
    local_data.my_slot->protected_node.exchange(p.unmark());
  }

  void release() {
    auto& slot = *local_data.my_slot;
    slot.protected_node.store(nullptr, std::memory_order_relaxed);
    slot.protected_src.store(nullptr, std::memory_order_relaxed);
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

  void free_node(node_type* node) {
    auto& slot = *local_data.my_slot;
    slot.free_nodes.push(node);
  }

 private:
  PARLAY_NOINLINE PARLAY_COLD void reclaim_free_nodes(AnnouncementSlot& slot) {

    if (slot.my_nodes != nullptr) {

      std::unordered_set<big_atomic_type*> protected_srcs;

      // ============================ Scan all actively announced big_atomics =============================
      for_each_slot([&](auto& announcement) { protected_srcs.insert(announcement.protected_src.load()); });

      for_each_node([&](auto* node) {
        if (node->src_.load() != nullptr) {

          node->pending_delayed_operation.store(true);  // <-- Tell the destructor to block
          big_atomic_type* src = node->src_.load();
          if (src != nullptr) {
            // The big_atomic src_ is now guaranteed to be alive.  Its destructor will block
            // until we complete this operation and unset pending_delayed_operation.
            auto ver = src->version.load();
            auto current = src->backup_value.load();

            if (current.get_ptr() == node && current.is_marked() &&
                src->try_seqlock_and_store(ver, node->value, current)) {
              current = current.unmark();
            }

            if (current.get_ptr() == node && !current.is_marked() && !protected_srcs.count(src)) {
              // Uninstall the node since it is in the cache and the source is not announced.
              auto empty_ptr = marked_node_ptr_type::create_ptr(nullptr);
              src->backup_value.compare_exchange_strong(current, empty_ptr);
              node->was_installed = false;
            }

            node->pending_delayed_operation.store(false);
            if (node->owns_refcount.load(std::memory_order_relaxed) == true &&
                node->owns_refcount.exchange(false) == true) {
              src->refcount.fetch_sub(1);
            }
          }
          else {
            node->pending_delayed_operation.store(false);
          }

        } else {
          node->was_installed = false;
        }
      });

      // ============================== Scan all actively announced nodes =================================

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
    if (slot.free_nodes.empty()) {
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

}  // namespace internal

template<typename T, typename Equal>
struct big_atomic {
  // T must be trivially copyable, but it doesn't have to be trivially default constructible (or
  // even default constructible at all) since we only make copies from what the user gives us
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(std::is_invocable_r_v<bool, Equal, T&&, T&&>);

  using node_type = internal::big_atomic_node<T, Equal>;
  using marked_node_ptr_type = marked_ptr<node_type>;
  using node_manager_type = internal::NodeManager<T, Equal>;

  friend node_type;
  friend node_manager_type;

 public:
  using value_type = T;
  using version_type = int64_t;

  big_atomic() : backup_value(marked_node_ptr_type::create_ptr(nullptr)), version(0), fast_value{}, refcount{0} {
    static_assert(std::is_default_constructible_v<T>);
    node_manager();
    new (static_cast<void*>(&fast_value)) T{};
  }

  /* implicit */ big_atomic(const T& t) :  // NOLINT(google-explicit-constructor)
      version(0), backup_value(marked_node_ptr_type::create_ptr(nullptr)), fast_value{}, refcount{0} {
    node_manager();
    new (static_cast<void*>(&fast_value)) T{t};
  }

  PARLAY_INLINE T load() noexcept {
    auto ver = version.load(std::memory_order_acquire);
    alignas(T) char buffer[sizeof(T)];
    atomic_load_per_byte_memcpy(&buffer, &fast_value, sizeof(T));
    auto p = backup_value.load();
    if (!p.is_marked() && ver == version.load(std::memory_order_relaxed)) [[likely]]
      return bits_to_object<T>(buffer);

    protect_this();
    return load_indirect(p);
  }

  PARLAY_INLINE void store(const T& desired) {
    auto num = version.load(std::memory_order_acquire);
    auto new_p = make_node(desired).mark();
    auto old_p = backup_value.exchange(new_p);
    if (old_p.get_ptr() != nullptr) {
      uninstall_backup(old_p);
    }
    try_seqlock_and_store(num, desired, new_p);
  }

  PARLAY_INLINE bool cas(const T& expected, const T& desired) {
    protect_this();
    auto p = protect_backup();
    auto ver = version.load(std::memory_order_acquire);
    alignas(T) char buffer[sizeof(T)];
    atomic_load_per_byte_memcpy(&buffer, &fast_value, sizeof(T));

    T current = [&]() {  // Use IIFE since you can't stick [[likely]] on a ternary statement
      if (!p.is_marked() && ver == version.load(std::memory_order_relaxed)) [[likely]]
        return bits_to_object<T>(buffer);
      else
        return load_indirect(p);
    }();

    if (!Equal{}(current, expected)) return false;
    if (Equal{}(expected, desired)) return true;

    auto new_p = make_node(desired).mark();
    auto old_p = p;

    if ((backup_value.load(std::memory_order_relaxed) == p && backup_value.compare_exchange_strong(p, new_p)) ||
        (p == backup_value.load(std::memory_order_relaxed) && old_p.get_ptr() != nullptr && p == old_p.unmark() &&
         backup_value.compare_exchange_strong(p, new_p))) {

      if (p.get_ptr() != nullptr) {
        uninstall_backup(p);
      }

      try_seqlock_and_store(ver, desired, new_p);
      return true;
    } else {
      free_node(new_p);
      return false;
    }
  }

  ~big_atomic() {
    auto p = backup_value.load();
    if (p.get_ptr() != nullptr) {
      announce(p);
      p->src_.store(nullptr);
      while (p->pending_delayed_operation.load() == true) { }
    }
    if (refcount.load() > 0) { std::cerr << "Waiting on refcount" << std::endl; }
    while (refcount.load() > 0) { }
  }

 private:
  marked_node_ptr_type make_node(const T& desired) noexcept {
    auto new_node = node_manager().get_free_node();
    new_node->value = desired;
    new_node->src_.store(this, std::memory_order_relaxed);
    return marked_node_ptr_type::create_ptr(new_node);
  }

  static void free_node(marked_node_ptr_type ptr) noexcept { node_manager().free_node(ptr.get_ptr()); }

  struct release_guard {
    ~release_guard() { release(); }
  };

  static void release() noexcept { node_manager().release(); }

  marked_node_ptr_type protect_backup() noexcept { return node_manager().protect_node(backup_value); }

  void protect_this() noexcept { node_manager().protect_src(this); }

  void announce(marked_node_ptr_type p) const noexcept { node_manager().announce(p); }

  // Requires protect_this() has already been called by the caller
  PARLAY_INLINE T load_indirect(marked_node_ptr_type& p) noexcept {
    p = protect_backup();
    if (p.get_ptr() != nullptr) [[likely]]
      return p->value;
    auto ver = version.load(std::memory_order_acquire);
    alignas(T) char buffer[sizeof(T)];
    atomic_load_per_byte_memcpy(&buffer, &fast_value, sizeof(T));
    p = protect_backup();
    if (!p.is_marked() && ver == version.load(std::memory_order_relaxed)) return bits_to_object<T>(buffer);
    assert(p.get_ptr() != nullptr);  // After the second try, it is guaranteed that the ptr is
    return p->value;                 // not uninstalled because the big_atomic was protected.
  }

  bool try_seqlock_and_store(version_type num, const T& desired, marked_node_ptr_type p) noexcept {
    assert(p.get_ptr() != nullptr);
    if ((num % 2 == 0) && num == version.load(std::memory_order_relaxed) &&
        version.compare_exchange_strong(num, num + 1)) {
      atomic_store_per_byte_memcpy(&fast_value, &desired, sizeof(T));
      version.store(num + 2, std::memory_order_release);
      return backup_value.compare_exchange_strong(p, p.unmark());
    }
    return false;
  }

  void uninstall_backup(marked_node_ptr_type p) noexcept {
    assert(p.get_ptr() != nullptr);
    announce(p);
    p->src_.store(nullptr);
    if (p->pending_delayed_operation.load() == true) {
      assert(p->owns_refcount.load() == false);
      refcount.fetch_add(1);
      p->owns_refcount.store(true);
    }
    if (p->pending_delayed_operation.load() == false && p->owns_refcount.load(std::memory_order_relaxed) == true &&
        p->owns_refcount.exchange(false) == true) {
      refcount.fetch_sub(1);
    }
  }

  static internal::NodeManager<T, Equal>& node_manager() { return internal::node_manager<T, Equal>(); }

  std::atomic<marked_node_ptr_type> backup_value{0};
  std::atomic<version_type> version;
  alignas(std::max_align_t) alignas(T) char fast_value[sizeof(T)];
  std::atomic<unsigned> refcount;  // Emergency refcount -- used only rarely
};

}  // namespace parlay
