// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/adapters/cuda_memory.hpp"

#include <cstring>
#include <mutex>
#include <string>

#include <cuda_runtime.h>

#include "coherence/adapters/region_store.hpp"
#include "coherence/version.hpp"

extern "C" int cf_cuda_launch_mutation(void* device_pointer, unsigned long long length,
                                       unsigned long long seed, unsigned int delta, void* stream,
                                       int* error_code);
extern "C" int cf_cuda_synchronize(void* stream);

namespace coherence {
namespace adapters {
namespace {

std::once_flag g_cuda_probe_once;
CudaCapability g_capability;

Status cuda_status(cudaError_t status, const char* what) {
  if (status == cudaSuccess) return Status::success();
  return Status(StatusCode::TransportFailure, what,
                std::string(cudaGetErrorName(status)) + ": " + cudaGetErrorString(status));
}

CudaCapability probe_once() {
  CudaCapability capability;
  int device_count = 0;
  const cudaError_t enumerated = cudaGetDeviceCount(&device_count);
  if (enumerated != cudaSuccess) {
    capability.available = false;
    capability.reason = std::string("no usable CUDA device: ") + cudaGetErrorString(enumerated);
    return capability;
  }
  if (device_count <= 0) {
    capability.available = false;
    capability.reason = "no CUDA device is present";
    return capability;
  }
  int runtime_version = 0;
  int driver_version = 0;
  (void)cudaRuntimeGetVersion(&runtime_version);
  (void)cudaDriverGetVersion(&driver_version);
  capability.runtime_version = runtime_version;
  capability.driver_version = driver_version;
  capability.toolkit = "cuda-runtime-" + std::to_string(runtime_version / 1000) + "." +
                       std::to_string((runtime_version % 1000) / 10);
  for (int index = 0; index < device_count; ++index) {
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, index) != cudaSuccess) continue;
    CudaDeviceInfo info;
    info.index = index;
    info.name = properties.name;
    info.compute_major = properties.major;
    info.compute_minor = properties.minor;
    info.total_memory = static_cast<std::uint64_t>(properties.totalGlobalMem);
    info.unified_addressing = properties.unifiedAddressing != 0;
    capability.devices.push_back(std::move(info));
  }
  capability.available = !capability.devices.empty();
  if (!capability.available) capability.reason = "no CUDA device could be described";
  return capability;
}

} // namespace

CudaCapability probe_cuda() {
  std::call_once(g_cuda_probe_once, [] { g_capability = probe_once(); });
  return g_capability;
}

std::string CudaCapability::render() const {
  std::string out;
  out.append("cuda available=");
  out.append(available ? "true" : "false");
  if (!available) {
    out.append(" reason=");
    out.append(reason);
    return out;
  }
  out.append(" toolkit=");
  out.append(toolkit);
  out.append(" driver=");
  out.append(std::to_string(driver_version));
  out.append(" runtime=");
  out.append(std::to_string(runtime_version));
  out.push_back('\n');
  for (const CudaDeviceInfo& device : devices) {
    out.append("  device ");
    out.append(std::to_string(device.index));
    out.append(" name=");
    out.append(device.name);
    out.append(" compute=");
    out.append(std::to_string(device.compute_major));
    out.push_back('.');
    out.append(std::to_string(device.compute_minor));
    out.append(" memory=");
    out.append(std::to_string(device.total_memory));
    out.append(" unified_addressing=");
    out.append(device.unified_addressing ? "true" : "false");
    out.push_back('\n');
  }
  return out;
}

void reference_mutation(MutableByteSpan bytes, std::uint64_t seed, std::uint32_t delta) {
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto key = static_cast<unsigned char>((seed >> (8u * static_cast<unsigned>(index % 8))) &
                                                0xFFull);
    const auto mixed = static_cast<unsigned char>(
        std::to_integer<std::uint8_t>(bytes[index]) ^ key ^
        static_cast<unsigned char>(index & 0xFFu));
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(
        mixed + static_cast<unsigned char>(delta)));
  }
}

CudaBuffer::~CudaBuffer() { release(); }

CudaBuffer::CudaBuffer(CudaBuffer&& other) noexcept
    : device_pointer_(other.device_pointer_),
      address_(other.address_),
      bytes_(other.bytes_),
      device_(other.device_) {
  other.device_pointer_ = nullptr;
  other.address_ = 0;
  other.bytes_ = 0;
}

CudaBuffer& CudaBuffer::operator=(CudaBuffer&& other) noexcept {
  if (this != &other) {
    release();
    device_pointer_ = other.device_pointer_;
    address_ = other.address_;
    bytes_ = other.bytes_;
    device_ = other.device_;
    other.device_pointer_ = nullptr;
    other.address_ = 0;
    other.bytes_ = 0;
  }
  return *this;
}

void CudaBuffer::release() noexcept {
  if (device_pointer_ != nullptr) {
    (void)cudaFree(device_pointer_);
    device_pointer_ = nullptr;
  }
  address_ = 0;
  bytes_ = 0;
}

bool CudaBuffer::valid() const noexcept { return device_pointer_ != nullptr; }

Result<CudaBuffer> CudaBuffer::allocate(std::uint64_t bytes, int device_index) {
  if (bytes == 0) {
    return Result<CudaBuffer>::failure(
        Status(StatusCode::InvalidArgument, "a device allocation size is required"));
  }
  const CudaCapability capability = probe_cuda();
  if (!capability.available) {
    return Result<CudaBuffer>::failure(
        Status(StatusCode::Unsupported, "CUDA is not available", capability.reason));
  }
  cudaError_t selected = cudaSetDevice(device_index);
  if (selected != cudaSuccess) {
    return Result<CudaBuffer>::failure(
        cuda_status(selected, "cannot select the requested CUDA device"));
  }
  CudaBuffer buffer;
  void* pointer = nullptr;
  const cudaError_t allocated = cudaMalloc(&pointer, static_cast<std::size_t>(bytes));
  if (allocated != cudaSuccess) {
    return Result<CudaBuffer>::failure(cuda_status(allocated, "cudaMalloc failed"));
  }
  buffer.device_pointer_ = pointer;
  buffer.address_ = reinterpret_cast<std::uint64_t>(pointer);
  buffer.bytes_ = bytes;
  buffer.device_ = device_index;
  return Result<CudaBuffer>::success(std::move(buffer));
}

Status CudaBuffer::synchronize() {
  if (!valid()) return Status(StatusCode::InvalidState, "the device buffer is not allocated");
  return cuda_status(cudaDeviceSynchronize(), "device synchronization failed");
}

Status CudaBuffer::upload(ByteSpan host) {
  if (!valid()) return Status(StatusCode::InvalidState, "the device buffer is not allocated");
  if (host.size() != bytes_) {
    return Status(StatusCode::InvalidArgument, "the upload size does not match the allocation",
                  std::to_string(host.size()) + " vs " + std::to_string(bytes_));
  }
  const cudaError_t copied =
      cudaMemcpy(device_pointer_, host.data(), static_cast<std::size_t>(bytes_),
                 cudaMemcpyHostToDevice);
  if (copied != cudaSuccess) return cuda_status(copied, "host-to-device copy failed");
  // A transfer is only complete once the stream has actually synchronized.
  return cuda_status(cudaStreamSynchronize(nullptr), "host-to-device synchronization failed");
}

Result<std::vector<std::byte>> CudaBuffer::download() const {
  if (!valid()) {
    return Result<std::vector<std::byte>>::failure(
        Status(StatusCode::InvalidState, "the device buffer is not allocated"));
  }
  std::vector<std::byte> host(static_cast<std::size_t>(bytes_));
  const cudaError_t copied = cudaMemcpy(host.data(), device_pointer_,
                                        static_cast<std::size_t>(bytes_),
                                        cudaMemcpyDeviceToHost);
  if (copied != cudaSuccess) {
    return Result<std::vector<std::byte>>::failure(
        cuda_status(copied, "device-to-host copy failed"));
  }
  const cudaError_t synced = cudaStreamSynchronize(nullptr);
  if (synced != cudaSuccess) {
    return Result<std::vector<std::byte>>::failure(
        cuda_status(synced, "device-to-host synchronization failed"));
  }
  return Result<std::vector<std::byte>>::success(std::move(host));
}

Status CudaBuffer::fill(std::uint64_t seed) {
  if (!valid()) return Status(StatusCode::InvalidState, "the device buffer is not allocated");
  std::vector<std::byte> pattern = make_pattern(bytes_, seed);
  return upload(ByteSpan(pattern));
}

Status CudaBuffer::mutate(std::uint64_t seed, std::uint32_t delta) {
  if (!valid()) return Status(StatusCode::InvalidState, "the device buffer is not allocated");
  int error_code = 0;
  const int launched = cf_cuda_launch_mutation(device_pointer_, bytes_, seed, delta, nullptr,
                                               &error_code);
  if (launched != 0) {
    return Status(StatusCode::InternalError, "the CUDA mutation kernel failed to launch",
                  std::to_string(error_code));
  }
  const int synchronized = cf_cuda_synchronize(nullptr);
  if (synchronized != 0) {
    return Status(StatusCode::InternalError, "the CUDA mutation kernel failed",
                  std::to_string(synchronized));
  }
  return Status::success();
}

Result<std::pair<std::uint64_t, std::uint64_t>> cuda_memory_info(int device_index) {
  const CudaCapability capability = probe_cuda();
  if (!capability.available) {
    return Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
        Status(StatusCode::Unsupported, "CUDA is not available", capability.reason));
  }
  (void)cudaSetDevice(device_index);
  std::size_t free_bytes = 0;
  std::size_t total_bytes = 0;
  const cudaError_t status = cudaMemGetInfo(&free_bytes, &total_bytes);
  if (status != cudaSuccess) {
    return Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
        cuda_status(status, "cudaMemGetInfo failed"));
  }
  return Result<std::pair<std::uint64_t, std::uint64_t>>::success(
      {static_cast<std::uint64_t>(free_bytes), static_cast<std::uint64_t>(total_bytes)});
}

} // namespace adapters
} // namespace coherence
