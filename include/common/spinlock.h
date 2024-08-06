#include <atomic>
#include <cstddef>
#include <vector>

#ifndef PARLAY_SPINLOCK_H_
#define PARLAY_SPINLOCK_H_

namespace parlay {

class spinlock {
 public:
  spinlock() : flag(0) { }

  void lock() {
    int delay = 5000;
    while (!try_lock()) {
        for (volatile int j = 0; j < delay; j++);
        delay = std::min(2 * delay, 20000);
    }
  }

  bool try_lock() {
    return flag.load(std::memory_order_relaxed) == 0 && flag.exchange(1, std::memory_order_acquire) == 0;
  }

  void unlock() {
    flag.store(0, std::memory_order_release);
  }

  [[nodiscard]] bool is_locked() const {
    return flag.load(std::memory_order_acquire);
  }

 private:
  std::atomic<unsigned> flag;
};

}  // namespace parlay

#endif  // PARLAY_SPINLOCK_H_
