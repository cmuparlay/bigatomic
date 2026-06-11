set(BENCHMARK_SOURCES bench_map.cpp)

set(BENCHMARK_BIG_ATOMICS "std" "simplock" "seqlock" "wf_direct" "wf_indirect" "wf_ablation" "lf_lowmem")
set(BENCHMARK_BIG_ATOMICS_LL "seqlock" "wf_direct" "wf_indirect" "wf_ablation" "lf_lowmem")
set(BENCHMARK_SIZES "1" "2" "4" "8" "16")

if(BENCHMARK_HTM)
  list(APPEND BENCHMARK_BIG_ATOMICS "htm")
  target_compile_options(bigatomic INTERFACE -mrtm)
endif()

foreach(ATOMIC_NAME ${BENCHMARK_BIG_ATOMICS})
  foreach(TABLE_NAME hash_array)
    foreach(SIZE ${BENCHMARK_SIZES})
      set(BENCH_NAME "bench_${TABLE_NAME}_${SIZE}_${ATOMIC_NAME}")
      add_executable(${BENCH_NAME} ${BENCHMARK_SOURCES})
      target_link_libraries(${BENCH_NAME} PRIVATE atomic bigatomic)
      target_compile_definitions(${BENCH_NAME} PRIVATE "NUM_WORDS=${SIZE}")
      target_include_directories(${BENCH_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${TABLE_NAME}")
      target_include_directories(${BENCH_NAME} PRIVATE "${PARLAY_BIGATOMIC_INCLUDE_DIR}/bigatomic/${ATOMIC_NAME}")
      if (NOT MSVC)
        target_compile_options(${BENCH_NAME} PRIVATE)
      endif()
    endforeach()
  endforeach()
endforeach()

foreach(ATOMIC_NAME ${BENCHMARK_BIG_ATOMICS_LL})
  foreach(TABLE_NAME parlay_hash)
    set(BENCH_NAME "bench_${TABLE_NAME}_${ATOMIC_NAME}")
    add_executable(${BENCH_NAME} ${BENCHMARK_SOURCES})
    target_link_libraries(${BENCH_NAME} PRIVATE atomic bigatomic)
    target_include_directories(${BENCH_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${TABLE_NAME}")
    target_include_directories(${BENCH_NAME} PRIVATE "${PARLAY_BIGATOMIC_INCLUDE_DIR}/bigatomic/${ATOMIC_NAME}")
  endforeach()
endforeach()

foreach(ATOMIC_NAME ${BENCHMARK_BIG_ATOMICS})
  foreach(TABLE_NAME leaf_tree)
    set(BENCH_NAME "bench_${TABLE_NAME}_${ATOMIC_NAME}")
    add_executable(${BENCH_NAME} ${BENCHMARK_SOURCES})
    target_link_libraries(${BENCH_NAME} PRIVATE atomic bigatomic)
    target_compile_definitions(${BENCH_NAME} PRIVATE "Ordered")
    target_include_directories(${BENCH_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${TABLE_NAME}")
    target_include_directories(${BENCH_NAME} PRIVATE "${PARLAY_BIGATOMIC_INCLUDE_DIR}/bigatomic/${ATOMIC_NAME}")
    if (NOT MSVC)
      target_compile_options(${BENCH_NAME} PRIVATE)
    endif()
  endforeach()
endforeach()

set(BENCH_NAME "bench_hash_list")
add_executable(${BENCH_NAME} ${BENCHMARK_SOURCES})
target_link_libraries(${BENCH_NAME} PRIVATE atomic bigatomic)
target_include_directories(${BENCH_NAME} PRIVATE "${PARLAY_BIGATOMIC_INCLUDE_DIR}/hash_list")

# Setbench is used is for natarajan-mittal trees
set(SET_BENCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/setbench/setbench)

set(SET_BENCH "natarajan_ext_bst_lf")

message(${SET_BENCH_DIR})

# A hack to get a list of include directories
execute_process(COMMAND find ${SET_BENCH_DIR}/common -type d OUTPUT_VARIABLE INCL_STRING)

string(REPLACE "\n" ";" INCL_LIST ${INCL_STRING})
set(DEBRA_OPTIONS -DUSE_TREE_STATS) # tree stats used for size

foreach(bench ${SET_BENCH})
  add_executable(${bench} ${BENCHMARK_SOURCES})
  target_link_libraries(${bench} PRIVATE atomic bigatomic)
  target_compile_definitions(${bench} PRIVATE "Ordered")
  target_include_directories(${bench} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/setbench")
  target_include_directories(${bench} PRIVATE ${SET_BENCH_DIR}/ds/${bench})
  target_include_directories(${bench} PRIVATE ${INCL_LIST})
  target_compile_options(${bench} PRIVATE -fpermissive ${DEBRA_OPTIONS})
endforeach()

function(other_bench NAME LINKS OPTIONS)
  set(NAME ${NAME})
  add_executable(${NAME} ${BENCHMARK_SOURCES})
  target_link_libraries(${NAME} PRIVATE atomic bigatomic)
  target_link_libraries(${NAME} PRIVATE ${LINKS})
  target_compile_options(${NAME} PRIVATE ${OPTIONS})
  target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/other/${NAME})
endfunction()

find_package(GTest)
find_package(absl)
other_bench(std_sharded "" "")
other_bench(libcuckoo  "" "")
other_bench(parallel_hashmap  "" "")
other_bench(seq_hash  "" "")

#add_subdirectory(abseil-cpp)
if (absl_FOUND)
  other_bench(abseil  absl::flat_hash_map "" "")
  other_bench(abseil_sharded  absl::flat_hash_map "" "")
endif()

find_package(folly)
if (folly_FOUND)
  other_bench(folly_hash  "folly;glog;dl;pthread;double-conversion;fmt;gflags;unwind" "")
  other_bench(folly_sharded  "folly;glog;dl;pthread;double-conversion;fmt;gflags;unwind" "")
  other_bench(folly_F14  "folly;glog;dl;pthread;double-conversion;fmt;gflags;unwind" "")
endif()

find_package(TBB)
IF (TBB_FOUND)
  other_bench(tbb_hash  TBB::tbb "" "")
endif()

set(BOOST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build/benchmarks/boost_1_83_0)

# We have to manually specify Boost's location since we are using the latest
# cutting-edge Boost version, and FindBoost.cmake can not find Boost.Unordered
# if (BOOST_DIR)
#   other_bench(boost_hash  "" "" "")
#   target_include_directories(boost_hash PRIVATE ${BOOST_DIR})
# endif()
