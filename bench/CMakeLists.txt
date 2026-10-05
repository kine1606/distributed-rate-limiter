find_package(benchmark REQUIRED)

add_executable(bench_core bench_core.cc)
target_include_directories(bench_core PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_core PRIVATE rl_api benchmark::benchmark benchmark::benchmark_main Threads::Threads)
