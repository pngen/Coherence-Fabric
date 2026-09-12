// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Narrow CUDA adapter.
//
// The core runtime never requires CUDA. This adapter exists so that a real
// device allocation, a real host-to-device transfer, a real kernel mutation and
// a real device-to-host transfer can be governed by the same coherence
// semantics as host memory.
//
// It does NOT implement or claim GPU hardware cache coherence. Memory is made
// coherent here by software: explicit transfers, explicit ownership transfer,
// explicit invalidation of the host-side copy and explicit resynchronization.
#ifndef COHERENCE_ADAPTERS_CUDA_MEMORY_HPP
#define COHERENCE_ADAPTERS_CUDA_MEMORY_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/enums.hpp"
#include "coherence/export.hpp"
#include "coherence/model.hpp"
#include "coherence/status.hpp"

namespace coherence {
namespace adapters {

struct COHERENCE_API CudaDeviceInfo {
  int index = 0;
  std::string name;
  int compute_major = 0;
  int compute_minor = 0;
  std::uint64_t total_memory = 0;
  bool unified_addressing = false;
};

struct COHERENCE_API CudaCapability {
  bool available = false;
  std::string reason;
  std::string toolkit;
  int driver_version = 0;
  int runtime_version = 0;
  std::vector<CudaDeviceInfo> devices;

  [[nodiscard]] std::string render() const;
};

/// Probe the platform. Returns available=false with a reason when no usable
/// device or runtime is present. The runtime must never claim a GPU proof it
/// did not perform, so this result is reported verbatim by the CLI.
[[nodiscard]] COHERENCE_API CudaCapability probe_cuda();

/// Deterministic host-side transform. The device kernel performs the identical
/// transform, which is what makes the parity check meaningful.
COHERENCE_API void reference_mutation(MutableByteSpan bytes, std::uint64_t seed,
                                      std::uint32_t delta);

class COHERENCE_API CudaBuffer {
 public:
  CudaBuffer() = default;
  ~CudaBuffer();
  CudaBuffer(const CudaBuffer&) = delete;
  CudaBuffer& operator=(const CudaBuffer&) = delete;
  CudaBuffer(CudaBuffer&& other) noexcept;
  CudaBuffer& operator=(CudaBuffer&& other) noexcept;

  [[nodiscard]] static Result<CudaBuffer> allocate(std::uint64_t bytes, int device_index);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t device_address() const noexcept { return address_; }
  [[nodiscard]] int device_index() const noexcept { return device_; }

  /// Host-to-device transfer followed by a stream synchronization, so that a
  /// successful return really means the device has the bytes.
  Status upload(ByteSpan host);
  /// Device-to-host transfer followed by a stream synchronization.
  Result<std::vector<std::byte>> download() const;
  Status fill(std::uint64_t seed);
  /// Launch the deterministic mutation kernel and wait for it to complete.
  Status mutate(std::uint64_t seed, std::uint32_t delta);
  Status synchronize();

 private:
  void release() noexcept;
  void* device_pointer_ = nullptr;
  std::uint64_t address_ = 0;
  std::uint64_t bytes_ = 0;
  int device_ = 0;
};

/// Free and total device memory in bytes. Used by the proof to show that the
/// device baseline returns after cleanup.
COHERENCE_API Result<std::pair<std::uint64_t, std::uint64_t>> cuda_memory_info(int device_index);

} // namespace adapters
} // namespace coherence

#endif // COHERENCE_ADAPTERS_CUDA_MEMORY_HPP
