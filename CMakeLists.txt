cmake_minimum_required(VERSION 3.20)
project(DistributedRateLimiter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RL_NATIVE "Compile with -march=native (do not use for portable binaries/Docker images)" ON)
option(RL_BUILD_TESTS "Build unit tests" ON)
option(RL_BUILD_BENCH "Build benchmarks" ON)
set(RL_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread, or empty")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  if(RL_NATIVE)
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=native")
  endif()
  if(RL_SANITIZER)
    add_compile_options(-fsanitize=${RL_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${RL_SANITIZER})
  endif()
endif()

find_package(Threads REQUIRED)
find_package(hiredis CONFIG QUIET)
if(NOT hiredis_FOUND)
  find_library(HIREDIS_LIB hiredis REQUIRED)
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h REQUIRED)
  add_library(hiredis UNKNOWN IMPORTED)
  set_target_properties(hiredis PROPERTIES IMPORTED_LOCATION "${HIREDIS_LIB}"
                                           INTERFACE_INCLUDE_DIRECTORIES "${HIREDIS_INCLUDE_DIR}")
endif()
if(TARGET hiredis::hiredis)
  set(RL_HIREDIS hiredis::hiredis)
else()
  set(RL_HIREDIS hiredis)
endif()
# redis-plus-plus with async (libuv) support: Ubuntu does not package it, scripts/setup.sh builds it.
find_library(REDISPP_LIB redis++ REQUIRED)
find_path(REDISPP_INCLUDE_DIR sw/redis++/async_redis++.h REQUIRED)
find_library(UV_LIB uv REQUIRED)
find_package(OpenSSL REQUIRED)  # SHA1 of the Lua scripts
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

# --- protobuf + gRPC code generation ---------------------------------------------------
if(TARGET gRPC::grpc_cpp_plugin)
  set(GRPC_CPP_PLUGIN_LOCATION $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
else()
  find_program(GRPC_CPP_PLUGIN_LOCATION grpc_cpp_plugin REQUIRED)
endif()
if(TARGET protobuf::protoc)
  set(PROTOC_LOCATION $<TARGET_FILE:protobuf::protoc>)
else()
  find_program(PROTOC_LOCATION protoc REQUIRED)
endif()

set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_DIR})
set(PROTO_FILE "${PROTO_SRC_DIR}/rate_limiter.proto")
set(PROTO_OUTPUTS
  "${GENERATED_DIR}/rate_limiter.pb.cc" "${GENERATED_DIR}/rate_limiter.pb.h"
  "${GENERATED_DIR}/rate_limiter.grpc.pb.cc" "${GENERATED_DIR}/rate_limiter.grpc.pb.h")

add_custom_command(
  OUTPUT ${PROTO_OUTPUTS}
  COMMAND ${PROTOC_LOCATION}
  ARGS --proto_path "${PROTO_SRC_DIR}" --cpp_out "${GENERATED_DIR}" --grpc_out "${GENERATED_DIR}"
       "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_LOCATION}" "${PROTO_FILE}"
  DEPENDS "${PROTO_FILE}"
  COMMENT "Generating protobuf/gRPC stubs"
  VERBATIM)

add_library(rl_proto STATIC ${PROTO_OUTPUTS})
target_include_directories(rl_proto PUBLIC ${GENERATED_DIR})
target_link_libraries(rl_proto PUBLIC gRPC::grpc++ gRPC::grpc++_reflection protobuf::libprotobuf)

# --- libraries -------------------------------------------------------------------------
# Public headers (no gRPC/protobuf dependency allowed here).
add_library(rl_api INTERFACE)
target_include_directories(rl_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(rl_config STATIC src/config/rule_config.cc)
target_include_directories(rl_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rl_config PUBLIC rl_api yaml-cpp::yaml-cpp)

add_library(rl_redis STATIC src/redis/redispp_backend.cc src/redis/batcher.cc src/redis/plan_listener.cc)
target_include_directories(rl_redis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${REDISPP_INCLUDE_DIR})
target_link_libraries(rl_redis PUBLIC rl_api ${REDISPP_LIB} ${RL_HIREDIS} ${UV_LIB} OpenSSL::Crypto spdlog::spdlog
                                      Threads::Threads)

add_library(rl_engine STATIC src/lease/lease_table.cc src/core/leased_core.cc src/core/composite_core.cc
                             src/plan/plan_store.cc src/usage/usage_log.cc)
target_include_directories(rl_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rl_engine PUBLIC rl_api Threads::Threads)

# --- executables -----------------------------------------------------------------------
add_executable(rate_limiter_server src/server/main.cc)
target_include_directories(rate_limiter_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rate_limiter_server PRIVATE rl_proto rl_api rl_config rl_redis rl_engine spdlog::spdlog Threads::Threads)

if(RL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(RL_BUILD_BENCH)
  add_subdirectory(bench)
endif()
