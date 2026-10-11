find_package(GTest REQUIRED)

add_executable(rl_tests config_test.cc core_test.cc strict_core_test.cc lease_test.cc leased_core_test.cc
                        hiredis_backend_test.cc lua_scripts_test.cc)
target_include_directories(rl_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rl_tests PRIVATE RL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(rl_tests PRIVATE rl_config rl_redis rl_engine rl_api GTest::gtest_main Threads::Threads ${RL_HIREDIS})

include(GoogleTest)
gtest_discover_tests(rl_tests)
