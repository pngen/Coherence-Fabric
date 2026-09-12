# Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

set(COHERENCE_FABRIC_TEST_TARGETS "")

function(coherence_fabric_add_test name)
  cmake_parse_arguments(CF "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${CF_SOURCES})
  target_link_libraries(${name} PRIVATE CoherenceFabric::coherence_fabric ${CF_LIBS})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  coherence_fabric_apply_warnings(${name})
  coherence_fabric_apply_sanitizer(${name})
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "tests")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/test-work"
    ENVIRONMENT "COHERENCE_FABRIC_BINDIR=${CMAKE_BINARY_DIR}/bin")
  set(COHERENCE_FABRIC_TEST_TARGETS ${COHERENCE_FABRIC_TEST_TARGETS} ${name} PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-work")

coherence_fabric_add_test(cf_test_core SOURCES unit_core.cpp)
coherence_fabric_add_test(cf_test_semantics SOURCES unit_semantics.cpp)
coherence_fabric_add_test(cf_test_protocol SOURCES unit_protocol.cpp)
coherence_fabric_add_test(cf_test_persistence SOURCES test_persistence.cpp)
coherence_fabric_add_test(cf_test_property SOURCES test_property.cpp)
coherence_fabric_add_test(cf_test_concurrency SOURCES test_concurrency.cpp)
coherence_fabric_add_test(cf_test_adversarial SOURCES test_adversarial.cpp)
coherence_fabric_add_test(cf_test_memory SOURCES test_memory.cpp)
coherence_fabric_add_test(cf_test_invariants SOURCES test_invariants.cpp)
coherence_fabric_add_test(cf_test_distributed SOURCES test_distributed.cpp)
coherence_fabric_add_test(cf_test_scale SOURCES test_scale.cpp)

if(COHERENCE_FABRIC_CUDA_ENABLED)
  coherence_fabric_add_test(cf_test_cuda SOURCES test_cuda.cpp LIBS CoherenceFabric::coherence_fabric_cuda)
endif()

if(COHERENCE_FABRIC_ENABLE_ASAN)
  # Genuine-instrumentation probe. It is not registered with CTest because it
  # deliberately triggers a defect in a child process; the ASan test suite runs
  # it and asserts that the sanitizer actually reported the access.
  add_executable(cf_asan_probe asan_probe.cpp)
  target_link_libraries(cf_asan_probe PRIVATE CoherenceFabric::coherence_fabric)
  coherence_fabric_apply_warnings(cf_asan_probe)
  coherence_fabric_apply_sanitizer(cf_asan_probe)
  set_target_properties(cf_asan_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "tests")
  target_compile_definitions(cf_test_adversarial PRIVATE COHERENCE_FABRIC_ASAN_PROBE_PATH="$<TARGET_FILE:cf_asan_probe>")
endif()
