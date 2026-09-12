// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Real byte backing for registered replicas.
//
// Coherence Fabric is not an allocator, and RegionStore is not a general
// memory allocator API. It exists so that a participant process can hold the
// actual bytes a coherence claim is about: a pageable host buffer, a named
// shared mapping visible to another process, or a synthetic fixture. It also
// computes the content fingerprints the runtime uses to refuse to certify
// currentness when the bytes do not match.
#ifndef COHERENCE_ADAPTERS_REGION_STORE_HPP
#define COHERENCE_ADAPTERS_REGION_STORE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coherence/enums.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/model.hpp"
#include "coherence/status.hpp"

namespace coherence {
namespace adapters {

/// A mapped shared segment. Two processes that open the same name observe the
/// same physical pages, which is what makes a cross-process byte proof real
/// rather than simulated.
class COHERENCE_API SharedSegment {
 public:
  SharedSegment() = default;
  ~SharedSegment();
  SharedSegment(const SharedSegment&) = delete;
  SharedSegment& operator=(const SharedSegment&) = delete;
  SharedSegment(SharedSegment&& other) noexcept;
  SharedSegment& operator=(SharedSegment&& other) noexcept;

  /// Create or open a named segment. When create is true the segment is
  /// initialised to zero; otherwise the existing contents are observed.
  [[nodiscard]] static Result<SharedSegment> open(const std::string& name, std::uint64_t length,
                                                  bool create);
  [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
  [[nodiscard]] std::uint64_t length() const noexcept { return length_; }
  [[nodiscard]] std::byte* data() noexcept { return base_; }
  [[nodiscard]] const std::byte* data() const noexcept { return base_; }
  [[nodiscard]] ByteSpan span() const noexcept { return ByteSpan(base_, static_cast<std::size_t>(length_)); }

 private:
  void release() noexcept;
  void* mapping_ = nullptr;
  void* handle_ = nullptr;
  std::byte* base_ = nullptr;
  std::uint64_t length_ = 0;
  std::string name_;
};

enum class RegionBacking : std::uint8_t {
  HostHeap = 0,
  SharedMapping = 1,
  Synthetic = 2,
  /// Allocated by a device adapter (for example CUDA). The bytes live outside
  /// this process's pageable address space.
  DeviceLocal = 3,
};

struct COHERENCE_API StoredRegion {
  RegionId id;
  std::string name;
  MemoryDomain memory_domain = MemoryDomain::HostPageable;
  EvidenceClass evidence_class = EvidenceClass::Real;
  RegionBacking backing = RegionBacking::HostHeap;
  std::uint64_t length = 0;
  std::vector<std::byte> host;
  std::shared_ptr<SharedSegment> shared;
  std::uint64_t address_hint = 0;
  /// Set for device-backed regions: the adapter identity that owns the bytes.
  std::string device_adapter;

  [[nodiscard]] MutableByteSpan bytes();
  [[nodiscard]] ByteSpan bytes() const;
};

class COHERENCE_API RegionStore {
 public:
  RegionStore() = default;
  ~RegionStore() = default;
  RegionStore(const RegionStore&) = delete;
  RegionStore& operator=(const RegionStore&) = delete;

  /// A pageable host buffer filled with a deterministic pattern derived from
  /// the seed. Callers may overwrite the contents afterwards.
  Result<RegionId> add_host_region(std::string name, std::uint64_t length,
                                   MemoryDomain domain = MemoryDomain::HostPageable,
                                   std::uint64_t seed = 0,
                                   RegionId requested = RegionId::nil());

  /// A shared mapping. When create is true the segment is created and
  /// zero-filled; otherwise an existing segment is attached.
  Result<RegionId> add_shared_region(std::string name, const std::string& segment_name,
                                     std::uint64_t length, bool create);

  /// A synthetic CXL-class region. There is no physical CXL hardware behind it
  /// and the evidence class records that fact.
  Result<RegionId> add_synthetic_cxl_region(std::string name, std::uint64_t length,
                                            std::uint64_t seed);

  /// Register externally owned bytes (for example a CUDA allocation). The
  /// caller remains responsible for the lifetime of the allocation.
  Result<RegionId> attach_device_region(std::string name, std::string adapter,
                                        MemoryDomain domain, EvidenceClass evidence_class,
                                        std::uint64_t length, std::uint64_t address_hint);

  [[nodiscard]] StoredRegion* find(RegionId id);
  [[nodiscard]] const StoredRegion* find(RegionId id) const;
  [[nodiscard]] StoredRegion& at(RegionId id);
  [[nodiscard]] const StoredRegion& at(RegionId id) const;
  [[nodiscard]] std::size_t size() const noexcept { return regions_.size(); }
  [[nodiscard]] std::vector<RegionId> ids() const;

  /// Overwrite real bytes with a deterministic pattern.
  Status fill(RegionId id, std::uint64_t seed, std::uint64_t offset = 0,
              std::uint64_t length = 0);
  /// Apply a deterministic mutation to real bytes at a byte offset.
  Status poke(RegionId id, std::uint64_t offset, std::uint8_t value, std::uint64_t count = 1);

  [[nodiscard]] Result<ContentFingerprint> fingerprint(RegionId id) const;
  /// Copy real bytes between two regions of equal length and verify the copy by
  /// comparing the result fingerprint with the source fingerprint.
  Status copy_and_verify(RegionId source, RegionId destination);

 private:
  std::map<RegionId, StoredRegion> regions_;
  std::uint64_t next_id_ = 1;
};

/// Deterministic byte pattern used by tests and examples; not a random source.
COHERENCE_API std::vector<std::byte> make_pattern(std::uint64_t length, std::uint64_t seed);

} // namespace adapters
} // namespace coherence

#endif // COHERENCE_ADAPTERS_REGION_STORE_HPP
