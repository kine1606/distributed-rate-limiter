find_package(benchmark REQUIRED)

add_executable(bench_core bench_core.cc)
target_include_directories(bench_core PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_core PRIVATE rl_api benchmark::benchmark benchmark::benchmark_main Threads::Threads)

add_executable(bench_lease bench_lease.cc)
target_link_libraries(bench_lease PRIVATE rl_engine rl_api benchmark::benchmark benchmark::benchmark_main Threads::Threads)

# Envoy's rate limit API (the fields loadgen sends, wire-compatible), so one client loads both services.
set(RLS_OUT "${CMAKE_CURRENT_BINARY_DIR}/rls_min.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/rls_min.pb.h"
            "${CMAKE_CURRENT_BINARY_DIR}/rls_min.grpc.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/rls_min.grpc.pb.h")
add_custom_command(
  OUTPUT ${RLS_OUT}
  COMMAND ${PROTOC_LOCATION}
  ARGS --proto_path "${CMAKE_CURRENT_SOURCE_DIR}/loadgen" --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
       --grpc_out "${CMAKE_CURRENT_BINARY_DIR}" "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_LOCATION}"
       "${CMAKE_CURRENT_SOURCE_DIR}/loadgen/rls_min.proto"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/loadgen/rls_min.proto"
  VERBATIM)
add_executable(loadgen loadgen/loadgen.cc ${RLS_OUT})
target_include_directories(loadgen PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(loadgen PRIVATE rl_proto Threads::Threads)

add_executable(bench_customer bench_customer.cc)
target_link_libraries(bench_customer PRIVATE rl_engine rl_api benchmark::benchmark benchmark::benchmark_main Threads::Threads)

add_executable(bench_resilient bench_resilient.cc)
target_include_directories(bench_resilient PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bench_resilient PRIVATE rl_engine rl_api benchmark::benchmark benchmark::benchmark_main Threads::Threads)
