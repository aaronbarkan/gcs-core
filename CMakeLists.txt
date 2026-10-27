cmake_minimum_required(VERSION 3.16)
project(gcs_core LANGUAGES CXX)
# Keep editor include paths and compiler flags aligned with the actual build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Homebrew gRPC requires a newer standard; Debian's Docker build stays C++14.
if(NOT DEFINED CMAKE_CXX_STANDARD)
  if(APPLE)
    set(CMAKE_CXX_STANDARD 17)
  else()
    set(CMAKE_CXX_STANDARD 14)
  endif()
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
# Homebrew exports transitive dependencies through its config package.
# Debian 12 supplies FindProtobuf's module interface instead.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)
set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/telemetry.proto")
add_custom_command(
  OUTPUT telemetry.pb.cc telemetry.pb.h telemetry.grpc.pb.cc telemetry.grpc.pb.h
  COMMAND protobuf::protoc
  ARGS --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
       --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
       --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin> ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  VERBATIM)
add_library(telemetry_proto telemetry.pb.cc telemetry.grpc.pb.cc)
target_include_directories(telemetry_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(telemetry_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
add_library(telemetry_gateway TelemetryGateway.cpp TelemetrySubscriptions.cpp)
target_include_directories(telemetry_gateway PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(telemetry_gateway SYSTEM PUBLIC third_party/c_library_v2 ${Boost_INCLUDE_DIRS})
target_compile_definitions(telemetry_gateway PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
target_compile_options(telemetry_gateway PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(telemetry_gateway PUBLIC Threads::Threads)
add_executable(gcs-core main.cpp TelemetryProcessor.cpp TelemetryService.cpp TelemetryStateSubscriber.cpp TelemetryLogger.cpp)
target_compile_options(gcs-core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gcs-core PRIVATE telemetry_proto telemetry_gateway)

include(CTest)
if(BUILD_TESTING)
  add_executable(core-tests tests/core_tests.cpp TelemetryProcessor.cpp)
  target_include_directories(core-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(core-tests SYSTEM PRIVATE third_party/c_library_v2)
  target_link_libraries(core-tests PRIVATE Threads::Threads)
  add_test(NAME core-tests COMMAND core-tests)
  add_executable(gateway-tests tests/gateway_tests.cpp)
  target_link_libraries(gateway-tests PRIVATE telemetry_gateway)
  add_test(NAME gateway-tests COMMAND gateway-tests)
  set_tests_properties(gateway-tests PROPERTIES TIMEOUT 20)
  add_executable(logger-tests tests/logger_tests.cpp TelemetryLogger.cpp)
  target_link_libraries(logger-tests PRIVATE telemetry_gateway)
  add_test(NAME logger-tests COMMAND logger-tests)
  set_tests_properties(logger-tests PROPERTIES TIMEOUT 20)
endif()
