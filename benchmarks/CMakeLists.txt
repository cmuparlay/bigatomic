set(BENCHMARK_SOURCES bench_map.cpp)

set(BENCHMARK_BIG_ATOMICS "wf_loadonly" "seqlock" "simplock" "wf_epoch" "wf_hazard" "wf_lowmem" "wf_daniel" "wf_indirect" "std" "lf_lowmem")
set(BENCHMARK_TABLES "hash_array" "hash_list")

foreach(ATOMIC_NAME ${BENCHMARK_BIG_ATOMICS})
    foreach(TABLE_NAME ${BENCHMARK_TABLES})
        set(BENCH_NAME "bench_${TABLE_NAME}_${ATOMIC_NAME}")
        add_executable(${BENCH_NAME} ${BENCHMARK_SOURCES})
        target_link_libraries(${BENCH_NAME} PRIVATE atomic bigatomic)
        target_compile_definitions(${BENCH_NAME} PRIVATE "BIG_VALUE")
        target_include_directories(${BENCH_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${TABLE_NAME}")
        target_include_directories(${BENCH_NAME} PRIVATE "${PARLAY_BIGATOMIC_INCLUDE_DIR}/bigatomic/${ATOMIC_NAME}")

        if (NOT MSVC)
            target_compile_options(${BENCH_NAME} PRIVATE -Wfatal-errors)
        endif()
    endforeach()
endforeach()
