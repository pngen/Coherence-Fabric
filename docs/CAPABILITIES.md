# Capability matrix and validation environment

This document records exactly what was validated, on what, and what was not.

## Environment used for the 1.0.0 validation

| Item | Value |
| --- | --- |
| Operating system | Windows |
| Compiler | MSVC 19.44.35207 (x64), Visual Studio 2022 Build Tools |
| Warning configuration | `/W4 /WX /permissive- /Zc:__cplusplus /utf-8` |
| CMake | 4.3.2 |
| Generator | Ninja |
| CUDA toolkit | CUDA 12.9 (nvcc V12.9.86), runtime 12.90 |
| GPU | NVIDIA GeForce RTX 5090, compute capability 12.0 |
| Sanitizer | MSVC AddressSanitizer, x64 dynamic runtime |
| Network | loopback TCP only |

## REAL

| Capability | How it was proven |
| --- | --- |
| Windows process behaviour | `cf_test_distributed` launches the coordinator and two agents as real OS processes, kills them and restarts them |
| TCP loopback control plane | framed protocol tests plus the full multiprocess lifecycle over loopback |
| Host pageable memory | `cf_test_memory` allocates, mutates and compares real bytes |
| Host shared memory | a Windows named file mapping opened twice in `cf_test_memory`, and opened by separate agent processes in the multiprocess proof |
| Host pinned memory | `VirtualLock` probe in `cf_test_memory`, reported UNSUPPORTED when the platform refuses |
| Byte copy with content verification | every synchronization in the tests, examples and consumer verifies the destination fingerprint |
| Persistence | file store with CRC-32C and SHA-256, single-byte corruption and truncation rejection, real child process exit then reload |
| Coordinator restart | `cf_test_distributed` kills the coordinator mid-lifecycle and proves the epoch advances and pre-restart currentness is gone |
| Participant kill and reincarnation | the proof kills an agent while dirty, proves the old boot cannot mutate, and admits a fresh boot |
| RTX 5090 CUDA memory | `cf_test_cuda`: device probe, cudaMalloc, H2D, kernel mutation, stream synchronization, D2H, CPU parity, baseline return |

## SYNTHETIC

| Capability | Limit |
| --- | --- |
| CXL-class memory domain | ordinary host bytes labelled `cxl_class` with `synthetic` evidence; there is no CXL device |
| Remote memory byte path | the cross-process byte path is a Windows named file mapping on one host; the participant processes are real, the second physical node is not |

## UNSUPPORTED

Physical CXL hardware semantics; CXL.cache; CXL.mem device behaviour; fabric-attached pooled memory; CXL switches; RDMA-coherent memory; physical multi-node coherent memory; CPU hardware cache-protocol control; GPU hardware cache coherence; NVLink hardware coherence; declared multi-writer semantics (rejected at policy validation).

## Sanitizer limitations

MSVC AddressSanitizer supports heap, stack and global buffer overflows, use-after-free and
use-after-return on Windows. It does not support leak detection on this platform: the runtime
reports `detect_leaks is not supported on this platform`. Leak checking was therefore
performed structurally (RAII ownership, joined threads, closed sockets and handles, explicit
resource-closure assertions in the auditor and the cleanup phases of the tests) rather than by the
sanitizer. The CUDA adapter and the CUDA proof are excluded from the sanitizer build because CUDA
and AddressSanitizer cannot be used together; that path is covered by its own non-sanitized suite.

The probe executable `cf_asan_probe` deliberately performs an out-of-bounds write under
`--trigger`. The adversarial suite runs it in a child process and requires both a
non-zero exit and an `AddressSanitizer` report, which is what makes the instrumentation
claim genuine rather than assumed.
