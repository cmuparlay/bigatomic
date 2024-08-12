
#ifndef PARLAY_BIGATOMIC_HASH_LIST
#define PARLAY_BIGATOMIC_HASH_LIST

#include <functional>
#include <optional>
#include <tuple>
#include <array>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include <common/epoch.h>

#include "bigatomic.h"

namespace parlay {

template<typename K, typename V, class Hash = std::hash<K>, class KeyEqual = std::equal_to<K>>
struct unordered_map {

  // used internally for keeping keys and values without tickling constructors/destructors etc.
  using K_ = std::array<uint8_t,sizeof(K)>; // reloc
  using V_ = std::array<uint8_t,sizeof(V)>; // reloc

  // translate back to orginal type and destruct
  static void destruct_kv(const K_& k, const V_& v) { // reloc
    ((K*) (&k))->~K(); 
    ((V*) (&v))->~V(); 
  }

  // translate back to orginal type and retire (delayed)
  static void retire_kv(const K_& k, const V_& v) { // reloc
    epoch::get_default_retire_pool<K>().Retire((K*) (&k));
    epoch::get_default_retire_pool<V>().Retire((V*) (&v));
  }

  // translate to internal type with a copy
  static std::pair<K_, V_> copy_kv(const K& k, const V& v) { // reloc
    K_ kk; new (&kk) K(k); 
    V_ vv; new (&vv) V(v); 
    return std::pair(kk, vv);
  }

  // translate value back to original type with a copy
  static V get_value(const V_& v) { // reloc
    return *((V*) (&v));
  }

  static bool key_equal(const K_& a, const K& b) { // reloc
    K* ak = (K*) &a;
    return KeyEqual{}(*ak, b); 
  }
    
  struct link {
    K_ key;
    V_ value;
    link* next;
    link(const K_& key, const V_& value, link* next) : key(key), value(value), next(next) {}
    link() : next((link*)1) {}
    bool operator==(const link& o) const { return key == o.key && value == o.value && next == o.next; };
  };

  static link* set_empty() { return (link*)1; }
  static bool is_empty(const link* ptr) { return 1ul == (1ul & (size_t)ptr); }
  static link* strip_empty(const link* ptr) { return (link*)(~1ul & (size_t)ptr); }

  struct alignas(64) bucket {
    big_atomic<link> v;
  };

  struct Table {
    parlay::sequence<bucket> table;
    size_t size;
    big_atomic<link>* get_bucket(const K& k) {
      size_t idx = Hash{}(k) & (size - 1u);
      return &table[idx].v;
    }
    explicit Table(size_t n) {
      unsigned bits = 1 + parlay::log2_up(n);
      size = 1ul << bits;
      table = parlay::sequence<bucket>(size);
    }
  };

  Table hash_table;

  static link* new_link(const K_& k, const V_& v, link* l) { return epoch::New<link>(k, v, l); }

  explicit unordered_map(size_t n) : hash_table(Table(n)) {}
  ~unordered_map() {
    auto& table = hash_table.table;
    parlay::parallel_for(0, table.size(), [&](size_t i) { retire_list_all(table[i].v.load().next); });
  }

  static link* find_in_list(link* nxt_, const K& k) {
    link* nxt = nxt_;
    while (nxt != nullptr && key_equal(nxt->key, k)) {
      nxt = nxt->next;
    }
    return nxt;
  }

  // If k is found copies list elements up to k, and keeps the old
  // tail past k.  Returns the number of new nodes that will need to
  // be reclaimed, a pointer to the nead of he new list, and a pointer
  // to the link being removed.  If not found, does nothing, and
  // returns 0.
  static std::tuple<int, link*, link*> remove_from_list(link* nxt, const K& k) {
    if (nxt == nullptr)
      return std::tuple(0, nullptr, nullptr);
    else if (key_equal(nxt->key, k))
      return std::tuple(1, nxt->next, nxt);
    else {
      auto [len, head, to_remove] = remove_from_list(nxt->next, k);
      if (len == 0) return std::tuple(len, nullptr, nullptr);
      return std::tuple(len + 1, new_link(nxt->key, nxt->value, head), to_remove);
    }
  }

  // retires first n elements of a list
  static void retire_list_n(link* nxt, int n) {
    while (n > 0) {
      n--;
      link* tmp = nxt->next;
      epoch::Retire(nxt);
      nxt = tmp;
    }
  }

  static void retire_list_all(link* nxt) { 
    if (is_empty(nxt)) return;
    while (nxt != nullptr) {
      link* tmp = nxt->next;
      ((K*) &nxt->key)->~K();  // reloc
      ((V*) &nxt->value)->~V();  // reloc      
      epoch::Retire(nxt);
      nxt = tmp;
    }
  }

  static long list_length(const link& l) {
    link* nxt = l.next;
    if (is_empty(nxt)) return 0l;
    long len = 1;
    while (nxt != nullptr) {
      len++;
      nxt = nxt->next;
    }
    return len;
  }


  std::optional<V> find(const K& k) {
    auto s = hash_table.get_bucket(k);
    link l = s->load();
    if (is_empty(l.next)) return {};
    if (key_equal(l.key, k)) return get_value(l.value); // reloc
    return epoch::with_epoch([&]() -> std::optional<V> {
      link l = s->load();
      if (is_empty(l.next)) return {};
      if (key_equal(l.key, k)) return get_value(l.value); // reloc
      link* x = find_in_list(l.next, k);
      if (x == nullptr) return {};
      else return get_value(x->value); // reloc
    });
  }

  bool insert(const K& k, const V& v) {
    auto s = hash_table.get_bucket(k);
    __builtin_prefetch(s);
    return epoch::with_epoch([&] {
      long cnt = 0;
      auto [kk, vv] = copy_kv(k, v); // reloc
      while (true) {
        link l = s->load();
        if (is_empty(l.next)) {
          if (s->cas(l, link(kk, vv, nullptr))) return true;
        } else {
          if (key_equal(l.key, k)) {
            destruct_kv(kk, vv); // reloc
            return false;
          }
          if (find_in_list(l.next, k) != nullptr) return false;
          link* new_head = new_link(l.key, l.value, l.next);
          if (s->cas(l, link(kk, vv, new_head))) return true;
          epoch::Delete(new_head);
        }
        if (cnt++ > 1000000) {
          std::cout << "insert infinite loop?" << std::endl;
          abort();
        }
      }
    });
  }

  bool remove(const K& k) {
    auto s = hash_table.get_bucket(k);
    __builtin_prefetch(s);
    return epoch::with_epoch([&] {
      long cnt = 0;
      while (true) {
        link l = s->load();
        if (is_empty(l.next)) return false;
        if (key_equal(l.key, k)) {
          if (l.next == nullptr) {
            if (s->cas(l, link())) {
              retire_kv(l.key, l.value); // reloc
              return true;
            }
          } else {
            if (s->cas(l, link(l.next->key, l.next->value, l.next->next))) {
              epoch::Retire(l.next);
              retire_kv(l.key, l.value); // reloc
              return true;
            }
          }
        } else {
          auto [cnt, new_head, to_remove] = remove_from_list(l.next, k);
          if (cnt == 0) return false;
          if (s->cas(l, link(l.key, l.value, new_head))) {
            retire_kv(to_remove->key, to_remove->value); // reloc
            retire_list_n(l.next, cnt);
            return true;
          }
          retire_list_n(new_head, cnt - 1);
        }
        if (cnt++ > 1000000) {
          std::cout << "remove infinite loop?" << std::endl;
          abort();
        }
      }
    });
  }

  long size() {
    return parlay::reduce(parlay::map(hash_table.table, [&](bucket& x) { return list_length(x.v.load()); }));
  }
};

}  // namespace parlay
#endif  // PARLAY_BIGATOMIC_HASH_LIST
