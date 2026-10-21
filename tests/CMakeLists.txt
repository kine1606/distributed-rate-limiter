find_package(GTest REQUIRED)
include(GoogleTest)

# One executable per level, so `ctest -L unit|redis|integration` runs one level (tests/README.md).
# Unit: no network.
add_executable(rl_unit_tests config_test.cc convert_test.cc core_test.cc keys_test.cc plan_test.cc
                             strict_core_test.cc lease_test.cc leased_core_test.cc lease_harness_test.cc
                             batcher_test.cc deny_cache_test.cc composite_core_test.cc plan_store_test.cc
                             usage_log_test.cc service_test.cc circuit_breaker_test.cc
                             metrics_test.cc)
# Script: Lua scripts against a plain Redis (127.0.0.1:6380, or RL_TEST_REDIS).
add_executable(rl_redis_tests lua_scripts_test.cc)
# Integration: backend and gRPC against the dev cluster (127.0.0.1:7001, or RL_TEST_CLUSTER).
add_executable(rl_integration_tests redis_backend_it.cc grpc_quota_it.cc plan_change_it.cc leased_quota_it.cc)

foreach(t rl_unit_tests rl_redis_tests rl_integration_tests)
  target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_definitions(${t} PRIVATE RL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(${t} PRIVATE rl_config rl_redis rl_engine rl_api rl_proto GTest::gtest_main Threads::Threads
                                     ${RL_HIREDIS})
  if(RL_SANITIZER STREQUAL "thread")
    # GCC 13's TSan aborts ("unexpected memory mapping") under high ASLR entropy, as on WSL2's kernel.
    set_target_properties(${t} PROPERTIES CROSSCOMPILING_EMULATOR "setarch;-R")
  endif()
endforeach()
gtest_discover_tests(rl_unit_tests PROPERTIES LABELS unit)
gtest_discover_tests(rl_redis_tests PROPERTIES LABELS redis)
gtest_discover_tests(rl_integration_tests PROPERTIES LABELS integration)
