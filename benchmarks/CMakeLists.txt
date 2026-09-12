# Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

add_executable(coherence_benchmarks coherence_benchmarks.cpp)
target_link_libraries(coherence_benchmarks PRIVATE CoherenceFabric::coherence_fabric)
target_include_directories(coherence_benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
coherence_fabric_apply_warnings(coherence_benchmarks)
coherence_fabric_apply_sanitizer(coherence_benchmarks)
set_target_properties(coherence_benchmarks PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  FOLDER "benchmarks")
