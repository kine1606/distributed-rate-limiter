find_package(benchmark REQUIRED)

add_executable(bench_core bench_core.cc)
target_include_directories(bench_core PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_core PRIVATE rl_api benchmark::benchmark benchmark::benchmark_main Threads::Threads)

add_executable(bench_lease bench_lease.cc)
target_link_libraries(bench_lease PRIVATE rl_engine rl_api benchmark::benchmark benchmark::benchmark_main Threads::Threads)

add_executable(loadgen loadgen/loadgen.cc)
target_link_libraries(loadgen PRIVATE rl_proto Threads::Threads)
