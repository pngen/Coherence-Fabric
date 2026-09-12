# Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

set(COHERENCE_FABRIC_EXAMPLES
  exclusive_write_shared_reads
  host_replica_resync
  publication_versions
  shared_memory_participant
  stale_authority_rejection
  recovery_revalidation
  synthetic_cxl_domain
  strict_versus_relaxed_reads)

foreach(example IN LISTS COHERENCE_FABRIC_EXAMPLES)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE CoherenceFabric::coherence_fabric)
  target_include_directories(${example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  coherence_fabric_apply_warnings(${example})
  coherence_fabric_apply_sanitizer(${example})
  set_target_properties(${example} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "examples")
endforeach()

if(COHERENCE_FABRIC_CUDA_ENABLED)
  add_executable(cuda_host_device_coherence cuda_host_device_coherence.cpp)
  target_link_libraries(cuda_host_device_coherence
    PRIVATE CoherenceFabric::coherence_fabric CoherenceFabric::coherence_fabric_cuda)
  target_include_directories(cuda_host_device_coherence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  coherence_fabric_apply_warnings(cuda_host_device_coherence)
  set_target_properties(cuda_host_device_coherence PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "examples")
endif()
