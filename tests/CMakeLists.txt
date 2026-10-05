find_package(GTest REQUIRED)

add_executable(rl_tests config_test.cc core_test.cc)
target_include_directories(rl_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rl_tests PRIVATE RL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(rl_tests PRIVATE rl_config rl_api GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(rl_tests)
