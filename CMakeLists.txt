cmake_minimum_required(VERSION 3.16)
project(natspp VERSION 0.1.0 LANGUAGES CXX)

# --------------------------------------------------------------------------
# Global settings
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(NATSPP_BUILD_DEMO "Build demo executable" ON)
option(NATSPP_DEMO_PROTO "Generate and build protobuf demo sources" ON) # demo-only
include(CTest)  # defines BUILD_TESTING

# --------------------------------------------------------------------------
# Library (protobuf-free)
# --------------------------------------------------------------------------
add_library(natspp STATIC
  src/nats.cpp
)

target_include_directories(natspp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)
target_link_libraries(natspp PUBLIC Threads::Threads)

# If you ever need extra POSIX feature macros, uncomment:
# target_compile_definitions(natspp PRIVATE _POSIX_C_SOURCE=200809L)

# --------------------------------------------------------------------------
# Install rules + package config
# --------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS natspp
  EXPORT natsppTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/natsppConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

# You must have this file in your repo: cmake/natsppConfig.cmake.in
# Contents can be as simple as:
#   @PACKAGE_INIT@
#   include("${CMAKE_CURRENT_LIST_DIR}/natsppTargets.cmake")
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/natsppConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/natsppConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/natspp
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/natsppConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/natsppConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/natspp
)

install(EXPORT natsppTargets
  NAMESPACE natspp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/natspp
)

# --------------------------------------------------------------------------
# Demo (optional) – generates protobuf into the build tree
# --------------------------------------------------------------------------
if (NATSPP_BUILD_DEMO)
  add_executable(natspp_demo src/main.cpp)
  target_link_libraries(natspp_demo PRIVATE natspp)

  if (NATSPP_DEMO_PROTO)
    # Protobuf is required only for the demo with protos
    find_package(Protobuf REQUIRED)  # finds both lib and protoc

    # Proto sources & generated output dir (in build tree)
    set(PROTO_SRC_DIR ${CMAKE_SOURCE_DIR}/proto)
    set(PROTO_OUT_DIR ${CMAKE_BINARY_DIR}/proto)

    # List your .proto files here
    set(PROTO_FILES
      ${PROTO_SRC_DIR}/message.proto
    )

    file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

    # Map inputs to generated outputs (adjust names/paths as needed)
    # Example assumes message.proto -> message.pb.cc/.h (no subfolders)
    set(GEN_CC ${PROTO_OUT_DIR}/message.pb.cc)
    set(GEN_H  ${PROTO_OUT_DIR}/message.pb.h)

    add_custom_command(
      OUTPUT ${GEN_CC} ${GEN_H}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_OUT_DIR}
      COMMAND ${Protobuf_PROTOC_EXECUTABLE}
              --proto_path=${PROTO_SRC_DIR}
              --cpp_out=${PROTO_OUT_DIR}
              ${PROTO_FILES}
      DEPENDS ${PROTO_FILES}
      COMMENT "Generating C++ from protobufs"
      VERBATIM
    )

    # Object library to compile generated sources once
    add_library(natspp_protos OBJECT ${GEN_CC})
    target_include_directories(natspp_protos PUBLIC ${PROTO_OUT_DIR} ${Protobuf_INCLUDE_DIRS})

    # Demo links generated objects and protobuf runtime
    target_sources(natspp_demo PRIVATE $<TARGET_OBJECTS:natspp_protos>)
    add_dependencies(natspp_demo natspp_protos)

    # Prefer modern protobuf target if available
    if (TARGET protobuf::libprotobuf)
      target_link_libraries(natspp_demo PRIVATE protobuf::libprotobuf)
    else()
      target_link_libraries(natspp_demo PRIVATE ${Protobuf_LIBRARIES})
    endif()

    # Ensure demo sees generated headers
    target_include_directories(natspp_demo PRIVATE ${PROTO_OUT_DIR})
  endif()
endif()

# --------------------------------------------------------------------------
# Tests (optional) – only if sources exist or explicitly provided
# --------------------------------------------------------------------------
if (BUILD_TESTING)
  # Only add tests if your file exists (avoids failures in package builds)
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nats.cpp")
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    # MSVC recommends forcing shared CRT
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(natspp_tests tests/test_nats.cpp)
    target_link_libraries(natspp_tests PRIVATE natspp GTest::gtest_main)

    add_test(NAME natspp_tests COMMAND natspp_tests)
  endif()
endif()
