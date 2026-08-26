cmake_minimum_required(VERSION 3.20)
project(sluiced CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Werror)

find_package(Threads REQUIRED)

# --- the contract: headers only, no implementation ------------------------
add_library(sluiced_headers INTERFACE)
target_include_directories(sluiced_headers INTERFACE include)
target_compile_features(sluiced_headers INTERFACE cxx_std_20)

# --- gRPC / protobuf, needed once config_stream.hpp gets an implementation
# Guarded: on a machine without gRPC C++ installed (`brew install grpc
# protobuf`), configure must still succeed for the rest of the scaffold
# (headers + smoke test).
# Only probe for gRPC here — it pulls in its own matching Protobuf via
# find_dependency(). A separate find_package(Protobuf) before this one can
# resolve to a different, incompatible Protobuf install and make gRPC's own
# config script hard-error ("targets already defined") instead of just
# reporting not-found.
find_package(gRPC QUIET)
if(gRPC_FOUND)
  set(SLUICE_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/../proto/sluice/v1/config.proto)
  set(SLUICE_PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/genproto)
  set(SLUICE_PROTO_GEN_PKG_DIR ${SLUICE_PROTO_GEN_DIR}/sluice/v1)
  file(MAKE_DIRECTORY ${SLUICE_PROTO_GEN_PKG_DIR})
  get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)
  add_custom_command(
    OUTPUT
      ${SLUICE_PROTO_GEN_PKG_DIR}/config.pb.cc ${SLUICE_PROTO_GEN_PKG_DIR}/config.pb.h
      ${SLUICE_PROTO_GEN_PKG_DIR}/config.grpc.pb.cc ${SLUICE_PROTO_GEN_PKG_DIR}/config.grpc.pb.h
    COMMAND protobuf::protoc
    ARGS --cpp_out=${SLUICE_PROTO_GEN_DIR} --grpc_out=${SLUICE_PROTO_GEN_DIR}
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
         -I ${CMAKE_CURRENT_SOURCE_DIR}/../proto ${SLUICE_PROTO}
    DEPENDS ${SLUICE_PROTO}
  )
  add_library(sluiced_configpb
    ${SLUICE_PROTO_GEN_PKG_DIR}/config.pb.cc
    ${SLUICE_PROTO_GEN_PKG_DIR}/config.grpc.pb.cc)
  target_include_directories(sluiced_configpb PUBLIC ${SLUICE_PROTO_GEN_DIR})
  target_link_libraries(sluiced_configpb PUBLIC protobuf::libprotobuf gRPC::grpc++)
else()
  message(STATUS "sluiced: gRPC/Protobuf C++ not found — config_stream.hpp's "
                  "generated stubs won't build yet. `brew install grpc protobuf` "
                  "and re-run cmake once you start implementing it.")
endif()

# --- the implementation: Owen's, picked up automatically once it exists ---
file(GLOB SLUICED_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if(SLUICED_SRC)
  add_executable(sluiced ${SLUICED_SRC})
  target_link_libraries(sluiced PRIVATE sluiced_headers Threads::Threads)
  # config_stream.cpp compiles its gRPC Watch() client only when the generated
  # stubs are available; otherwise it serves the bootstrap set fail-static.
  if(TARGET sluiced_configpb)
    target_link_libraries(sluiced PRIVATE sluiced_configpb)
    target_compile_definitions(sluiced PRIVATE SLUICE_HAVE_GRPC)
  endif()
else()
  message(STATUS "sluiced: src/ is empty, not building the sluiced binary yet (see ROADMAP.md)")
endif()

# --- tests ------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(sluiced_test
  test/test_smoke.cpp
  test/test_maglev.cpp
  test/test_router.cpp
  test/test_stats.cpp
  test/test_health.cpp
  src/maglev.cpp
  src/router.cpp
  src/stats.cpp
  src/health_checker.cpp
)
target_link_libraries(sluiced_test PRIVATE sluiced_headers Threads::Threads Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(sluiced_test)
