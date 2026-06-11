
#ifndef PARLAY_BIGATOMIC_HASH_ARRAY
#define PARLAY_BIGATOMIC_HASH_ARRAY

#include <functional>
#include <optional>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include "bigatomic.h"

namespace parlay {

template<typename K, typename V, class Hash = std::hash<K>, class KeyEqual = std::equal_to<K>>
struct unordered_map {

  struct Voption {
    uint32_t flag{false};
    V value{0};
    Voption(bool flag, const V& value) : flag(flag), value(value) {}
    Voption(bool flag) : flag(flag) {}
    Voption() = default;
    bool operator==(const Voption& o) const { return flag == o.flag && value == o.value; };
  };

  struct alignas(64) entry {
    big_atomic<Voption> v;
  };

  parlay::sequence<entry> values;

  explicit unordered_map(std::size_t n) : values(parlay::sequence<entry>(2 * n)) { }

  std::optional<V> find(const K& k) {
    Voption x = values[k].v.load();
    if (x.flag) return x.value;
    else return {};
  }

  bool insert(const K& k, const V& v) {
    auto [old_v, tag] = values[k].v.ll();
    if (old_v.flag) return false;
    else return values[k].v.sc(tag, Voption(true, v));
  }

  bool remove(const K& k) {
    auto [old_v, tag] = values[k].v.ll();
    if (!old_v.flag) return false;
    else return values[k].v.sc(tag, Voption(false));
  }

  long size() {
    return parlay::reduce(parlay::map(values, [](entry& x) { return (long)x.v.load().flag; }));
  }
};

}  // namespace parlay
#endif  // PARLAY_BIGATOMIC_HASH_ARRAY
