// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// The device-side mutation kernel used by the real CUDA proof.
#include <cuda_runtime.h>

#include <cstdint>

namespace {

__global__ void cf_mutate_kernel(unsigned char* data, unsigned long long length,
                                 unsigned long long seed, unsigned int delta) {
  const unsigned long long index =
      static_cast<unsigned long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index >= length) return;
  const unsigned char key =
      static_cast<unsigned char>((seed >> (8u * static_cast<unsigned>(index % 8ull))) & 0xFFull);
  const unsigned char mixed =
      static_cast<unsigned char>(data[index] ^ key ^ static_cast<unsigned char>(index & 0xFFull));
  data[index] = static_cast<unsigned char>(mixed + static_cast<unsigned char>(delta));
}

} // namespace

extern "C" int cf_cuda_launch_mutation(void* device_pointer, unsigned long long length,
                                       unsigned long long seed, unsigned int delta,
                                       void* stream, int* error_code) {
  if (device_pointer == nullptr || length == 0) {
    if (error_code != nullptr) *error_code = 1;
    return 1;
  }
  const unsigned int threads = 256;
  const unsigned long long blocks = (length + threads - 1) / threads;
  if (blocks > 0x7FFFFFFFull) {
    if (error_code != nullptr) *error_code = 2;
    return 2;
  }
  cudaStream_t cuda_stream = reinterpret_cast<cudaStream_t>(stream);
  cf_mutate_kernel<<<static_cast<unsigned int>(blocks), threads, 0, cuda_stream>>>(
      static_cast<unsigned char*>(device_pointer), length, seed, delta);
  const cudaError_t launch = cudaGetLastError();
  if (error_code != nullptr) *error_code = static_cast<int>(launch);
  return launch == cudaSuccess ? 0 : static_cast<int>(launch);
}

extern "C" int cf_cuda_synchronize(void* stream) {
  const cudaError_t status = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream));
  return status == cudaSuccess ? 0 : static_cast<int>(status);
}
