# BigAtomics - Atomic Operartions on Multiple Adjacent Words

This library supports Load-Linked/Store-Conditional (LL/SC) and Load/CAS across multiple adjacent words.
The work is described in the paper:

> Big Atomics: Non-Blocking Algorithms with a Direct Fast Path.
> Daniel Anderson, Guy Blelloch, Zak Kent and Siddartha Jayanti.
> ACM Symposium on Parallelism in Algorithms (SPAA), 2026.

Please refer to the paper for more information.

We support the following interface:

```
template <typename V>
parlay::bigatomic {
  big_atomic(const V&);
  big_atomic();
  using tag = ...;
  std::pair<V,tag> ll();  // load linked
  bool sc(tag, const V&); // store conditional
  bool vl(tag);           // validate
  V load();
  bool cas(const V&, const V&);
}
```

We have several header only implementations:

- seqlock : a version based on sequence locks (not lock-free)
- simplock : a version based on traditional fine-grained locks (does not support LL/SC)
- std : a version that uses std::atomic<V> (does not support LL/SC)
- wf_indirect : a wait-free implementation that uses indirection
- wf_direct : a wait-free implementation that stores values directly in the atomic
- lf_lowmem : a lock-free implementation that stores values directly in the atomic and minimizes extra memory
- htm : an implementation based on hardware transactional memory (does not support LL/SC)

These can be found in `include/bigatomic/<name>/bigatomic.h`.

Experiments from the paper can be run as follows:

```
git clone git@github.com:cmuparlay/bigatomic.git
cd bigatomic
mkdir build
cd build
cmake ..
cd benchmarks
make -j
bash ../../benchmarks/runall.sh
```

This will create a file in `bigatomic/timings/` that includes all timings.  You can also run just one benchmark.  For example within the build/benchmarks directory:

```
./bench_hash_array_4_lf_lowmem -n 10000,10000000 -z 0,.99 -u 5,50
```

will run the lock-free low-memory implementation of 4-word big atomics for on two sizes (10K, 10M), with two Zipfian parameters (0, .99), and with two update percentages (5, 50).   It will print out both the times for the individual runs and the geometric mean.



