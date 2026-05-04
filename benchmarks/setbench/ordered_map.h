// Adapter to make setbench work in our benchmarks
#ifndef LLCODE_ADAPTER_H
#define LLCODE_ADAPTER_H

#include "random_fnv1a.h"
#include "adapter.h"

#include <parlay/primitives.h>
#include "allocator_parlay.h"
#include <optional>

thread_local int _tid = -1;
std::atomic<int> num_initialized_threads(0);

template <typename K_, typename V_>
class ordered_map {
public:
  using K = K_;
  using V = V_;

private:
  // use parlay allocator so has same allocator as ours
  using alloc = allocator_parlay<K>;
  using adapter_t = ds_adapter<K,V,reclaimer_debra<K>,alloc,pool_none<K>>;
  inline static const K KEY_NEG_INFTY = std::numeric_limits<K>::min()+1;
  inline static const K KEY_POS_INFTY = std::numeric_limits<K>::max()-1;
  adapter_t* my_map;

public:

  ordered_map(size_t n) {
    my_map = new adapter_t(parlay::num_workers(), KEY_NEG_INFTY, KEY_POS_INFTY, V(), nullptr);
  }

  std::optional<V> find(const K& key) {
    init_thread(my_map);
    V val = my_map->find(_tid, key);
    if (std::get<0>(val) == std::get<0>((V) my_map->getNoValue())) return {};
    else return val;
  } 

  bool insert(const K& key, const V& val) {
    init_thread(my_map);
    assert(key != 0);
    V r = my_map->insertIfAbsent(_tid, key, val); 
    return (std::get<0>(r) == std::get<0>((V) my_map->getNoValue()));
    //return (r == (V) my_map->getNoValue());
  }

  bool remove(const K key) {
    init_thread(my_map);
    V r = my_map->erase(_tid, key);
    return (std::get<0>(r) != std::get<0>((V) my_map->getNoValue()));
    //return (r != (V) my_map->getNoValue());
  }

  ~ordered_map() {
    init_thread(my_map);
    for(int i = 0; i < num_initialized_threads; i++)
      my_map->deinitThread(i); // all data structures have an additional check to avoid deinitializing twice
    // num_initialized_threads = 0;
    delete my_map;
  }

  size_t size() {
    init_thread(my_map);
    auto tree_stats = my_map->createTreeStats(KEY_NEG_INFTY, KEY_POS_INFTY);
    //std::cout << "average height: " << tree_stats->getAverageKeyDepth() << std::endl;
    size_t size = tree_stats->getKeys();
    delete tree_stats;
    return size;
  }

private:
  static inline void init_thread(adapter_t* ds) {
    if(_tid == -1) {
      _tid = num_initialized_threads.fetch_add(1);
      // std::cout << "thread id: " << _tid << std::endl;
    }
    ds->initThread(_tid);  // all data structures have an additional check to avoid initializing twice
  }
};

#endif // LLCODE_ADAPTER_H
