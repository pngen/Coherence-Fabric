# Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
cmake_minimum_required(VERSION 3.20)

project(CoherenceFabric
  VERSION 1.0.0
  DESCRIPTION "Open-source, vendor-neutral C++20 runtime for governing coherence, consistency, ownership, invalidation, synchronization, and authority across accelerator, host, CXL-class, and distributed memory."
  HOMEPAGE_URL "https://github.com/pngen/Coherence-Fabric"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(COHERENCE_FABRIC_IS_TOP_LEVEL ON)
else()
  set(COHERENCE_FABRIC_IS_TOP_LEVEL OFF)
endif()

option(COHERENCE_FABRIC_BUILD_TESTS "Build the Coherence Fabric test suites" ${COHERENCE_FABRIC_IS_TOP_LEVEL})
option(COHERENCE_FABRIC_BUILD_EXAMPLES "Build the Coherence Fabric examples" ${COHERENCE_FABRIC_IS_TOP_LEVEL})
option(COHERENCE_FABRIC_BUILD_BENCHMARKS "Build the Coherence Fabric benchmarks" ${COHERENCE_FABRIC_IS_TOP_LEVEL})
option(COHERENCE_FABRIC_BUILD_APPS "Build the coordinator, agent and CLI executables" ON)
option(COHERENCE_FABRIC_ENABLE_CUDA "Build the CUDA adapter and the real CUDA proof when a toolkit is present" ON)
option(COHERENCE_FABRIC_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(COHERENCE_FABRIC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning configuration. Applied to first-party targets only.
# ---------------------------------------------------------------------------
function(coherence_fabric_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(COHERENCE_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow
      -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast)
    if(COHERENCE_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(coherence_fabric_apply_sanitizer target)
  if(COHERENCE_FABRIC_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
    target_compile_definitions(${target} PRIVATE COHERENCE_FABRIC_ASAN=1)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# The runtime library.
# ---------------------------------------------------------------------------
set(COHERENCE_FABRIC_SOURCES
  src/bytes.cpp
  src/ids.cpp
  src/status.cpp
  src/version.cpp
  src/enums.cpp
  src/evidence.cpp
  src/policy.cpp
  src/model.cpp
  src/decision.cpp
  src/serialize.cpp
  src/codec.cpp
  src/codec_decisions.cpp
  src/platform_file.cpp
  src/persistence.cpp
  src/engine_core.cpp
  src/engine_registry.cpp
  src/engine_helpers.cpp
  src/engine_ops.cpp
  src/engine_ops2.cpp
  src/engine_ops3.cpp
  src/engine_recovery.cpp
  src/transport.cpp
  src/protocol.cpp
  src/client.cpp
  src/coordinator.cpp
  src/agent.cpp
  src/adapters/region_store.cpp)

add_library(coherence_fabric ${COHERENCE_FABRIC_SOURCES})
add_library(CoherenceFabric::coherence_fabric ALIAS coherence_fabric)

target_include_directories(coherence_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(coherence_fabric PUBLIC cxx_std_20)
set_target_properties(coherence_fabric PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

if(BUILD_SHARED_LIBS)
  target_compile_definitions(coherence_fabric
    PUBLIC COHERENCE_FABRIC_USE_SHARED
    PRIVATE COHERENCE_FABRIC_BUILD_SHARED)
endif()

if(WIN32)
  target_link_libraries(coherence_fabric PRIVATE ws2_32 bcrypt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(coherence_fabric PUBLIC Threads::Threads)
endif()

coherence_fabric_apply_warnings(coherence_fabric)
coherence_fabric_apply_sanitizer(coherence_fabric)

# ---------------------------------------------------------------------------
# Executables.
# ---------------------------------------------------------------------------
if(COHERENCE_FABRIC_BUILD_APPS)
  add_executable(coherence_coordinator apps/coherence_coordinator.cpp)
  target_link_libraries(coherence_coordinator PRIVATE CoherenceFabric::coherence_fabric)
  set_target_properties(coherence_coordinator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" FOLDER "apps")
  coherence_fabric_apply_warnings(coherence_coordinator)
  coherence_fabric_apply_sanitizer(coherence_coordinator)

  add_executable(coherence_agent apps/coherence_agent.cpp)
  target_link_libraries(coherence_agent PRIVATE CoherenceFabric::coherence_fabric)
  set_target_properties(coherence_agent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" FOLDER "apps")
  coherence_fabric_apply_warnings(coherence_agent)
  coherence_fabric_apply_sanitizer(coherence_agent)

  add_executable(coherence_cli apps/coherence_cli.cpp)
  target_include_directories(coherence_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
  target_link_libraries(coherence_cli PRIVATE CoherenceFabric::coherence_fabric)
  set_target_properties(coherence_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" FOLDER "apps")
  coherence_fabric_apply_warnings(coherence_cli)
  coherence_fabric_apply_sanitizer(coherence_cli)

  install(TARGETS coherence_coordinator coherence_agent coherence_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# CUDA adapter (optional, narrow, and never required by the core runtime).
# ---------------------------------------------------------------------------
set(COHERENCE_FABRIC_CUDA_ENABLED OFF)
if(COHERENCE_FABRIC_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
      set(COHERENCE_FABRIC_CUDA_ENABLED ON)
    endif()
  endif()
endif()

if(COHERENCE_FABRIC_CUDA_ENABLED)
  add_library(coherence_fabric_cuda src/adapters/cuda_memory.cpp src/adapters/cuda_probe_kernel.cu)
  add_library(CoherenceFabric::coherence_fabric_cuda ALIAS coherence_fabric_cuda)
  target_link_libraries(coherence_fabric_cuda PUBLIC CoherenceFabric::coherence_fabric CUDA::cudart)
  target_include_directories(coherence_fabric_cuda PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
  set_target_properties(coherence_fabric_cuda PROPERTIES
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_SEPARABLE_COMPILATION OFF
    POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    # Host flags go to the C++ translation unit only. nvcc would otherwise treat
    # them as input file names.
    target_compile_options(coherence_fabric_cuda PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/Zc:__cplusplus;/utf-8>)
    if(COHERENCE_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(coherence_fabric_cuda PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
    endif()
  endif()
  # Compile for the architectures actually present on the build host, which is
  # what makes the CUDA proof a real device proof rather than a simulated one.
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
      set(CMAKE_CUDA_ARCHITECTURES native CACHE STRING "CUDA architectures to build for")
    else()
      set(CMAKE_CUDA_ARCHITECTURES 52 CACHE STRING "CUDA architectures to build for")
    endif()
  endif()
  target_compile_definitions(coherence_fabric_cuda PUBLIC COHERENCE_FABRIC_HAS_CUDA=1)
  install(TARGETS coherence_fabric_cuda EXPORT CoherenceFabricTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Tests, examples and benchmarks.
# ---------------------------------------------------------------------------
if(COHERENCE_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(COHERENCE_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(COHERENCE_FABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Installation and package configuration.
# ---------------------------------------------------------------------------
install(TARGETS coherence_fabric
  EXPORT CoherenceFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(COHERENCE_FABRIC_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/CoherenceFabric)

install(EXPORT CoherenceFabricTargets
  FILE CoherenceFabricTargets.cmake
  NAMESPACE CoherenceFabric::
  DESTINATION ${COHERENCE_FABRIC_CMAKE_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoherenceFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CoherenceFabricConfig.cmake
  INSTALL_DESTINATION ${COHERENCE_FABRIC_CMAKE_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CoherenceFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CoherenceFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CoherenceFabricConfigVersion.cmake
  DESTINATION ${COHERENCE_FABRIC_CMAKE_DIR})

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/CoherenceFabric)

message(STATUS "Coherence Fabric ${PROJECT_VERSION}")
message(STATUS "  build type            : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests                 : ${COHERENCE_FABRIC_BUILD_TESTS}")
message(STATUS "  examples              : ${COHERENCE_FABRIC_BUILD_EXAMPLES}")
message(STATUS "  benchmarks            : ${COHERENCE_FABRIC_BUILD_BENCHMARKS}")
message(STATUS "  applications          : ${COHERENCE_FABRIC_BUILD_APPS}")
message(STATUS "  CUDA adapter          : ${COHERENCE_FABRIC_CUDA_ENABLED}")
message(STATUS "  AddressSanitizer      : ${COHERENCE_FABRIC_ENABLE_ASAN}")
