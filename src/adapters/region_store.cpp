// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/adapters/region_store.hpp"

#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace coherence {
namespace adapters {

std::vector<std::byte> make_pattern(std::uint64_t length, std::uint64_t seed) {
  std::vector<std::byte> out(static_cast<std::size_t>(length));
  DeterministicRandom random(seed == 0 ? 0x9E3779B97F4A7C15ull : seed);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(random.next_u32() & 0xFFu);
  }
  return out;
}

SharedSegment::~SharedSegment() { release(); }

SharedSegment::SharedSegment(SharedSegment&& other) noexcept
    : mapping_(other.mapping_),
      handle_(other.handle_),
      base_(other.base_),
      length_(other.length_),
      name_(std::move(other.name_)) {
  other.mapping_ = nullptr;
  other.handle_ = nullptr;
  other.base_ = nullptr;
  other.length_ = 0;
}

SharedSegment& SharedSegment::operator=(SharedSegment&& other) noexcept {
  if (this != &other) {
    release();
    mapping_ = other.mapping_;
    handle_ = other.handle_;
    base_ = other.base_;
    length_ = other.length_;
    name_ = std::move(other.name_);
    other.mapping_ = nullptr;
    other.handle_ = nullptr;
    other.base_ = nullptr;
    other.length_ = 0;
  }
  return *this;
}

void SharedSegment::release() noexcept {
#if defined(_WIN32)
  if (base_ != nullptr) {
    ::UnmapViewOfFile(base_);
    base_ = nullptr;
  }
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (base_ != nullptr) {
    ::munmap(base_, static_cast<std::size_t>(length_));
    base_ = nullptr;
  }
  if (handle_ != nullptr) {
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)));
    handle_ = nullptr;
  }
#endif
  length_ = 0;
}

Result<SharedSegment> SharedSegment::open(const std::string& name, std::uint64_t length,
                                          bool create) {
  if (name.empty()) {
    return Result<SharedSegment>::failure(
        Status(StatusCode::InvalidArgument, "a shared segment name is required"));
  }
  if (length == 0) {
    return Result<SharedSegment>::failure(
        Status(StatusCode::InvalidArgument, "a shared segment length is required"));
  }
  SharedSegment segment;
  segment.name_ = name;
  segment.length_ = length;
#if defined(_WIN32)
  const std::wstring wide(name.begin(), name.end());
  HANDLE handle = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       static_cast<DWORD>(length >> 32),
                                       static_cast<DWORD>(length & 0xFFFFFFFFu),
                                       wide.c_str());
  if (handle == nullptr) {
    return Result<SharedSegment>::failure(
        Status(StatusCode::ResourceExhausted, "cannot create the shared mapping",
               name + " win32=" + std::to_string(::GetLastError())));
  }
  const bool created_now = ::GetLastError() != ERROR_ALREADY_EXISTS;
  if (create && !created_now) {
    // The caller asked to create but the segment already existed: reject rather
    // than silently reusing another run's bytes.
    ::CloseHandle(handle);
    return Result<SharedSegment>::failure(
        Status(StatusCode::DuplicateIdentity,
               "a shared segment with this name already exists and creation was requested", name));
  }
  void* view = ::MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(length));
  if (view == nullptr) {
    ::CloseHandle(handle);
    return Result<SharedSegment>::failure(
        Status(StatusCode::ResourceExhausted, "cannot map the shared segment",
               name + " win32=" + std::to_string(::GetLastError())));
  }
  segment.handle_ = handle;
  segment.mapping_ = view;
  segment.base_ = static_cast<std::byte*>(view);
#else
  const int fd = ::shm_open(name.c_str(), create ? (O_CREAT | O_RDWR | O_EXCL) : O_RDWR, 0600);
  if (fd < 0) {
    return Result<SharedSegment>::failure(
        Status(StatusCode::ResourceExhausted, "cannot open the shared segment", name));
  }
  if (create && ::ftruncate(fd, static_cast<off_t>(length)) != 0) {
    ::close(fd);
    ::shm_unlink(name.c_str());
    return Result<SharedSegment>::failure(
        Status(StatusCode::ResourceExhausted, "cannot size the shared segment", name));
  }
  void* view = ::mmap(nullptr, static_cast<std::size_t>(length), PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
  if (view == MAP_FAILED) {
    ::close(fd);
    return Result<SharedSegment>::failure(
        Status(StatusCode::ResourceExhausted, "cannot map the shared segment", name));
  }
  segment.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  segment.mapping_ = view;
  segment.base_ = static_cast<std::byte*>(view);
#endif
  if (create) std::memset(segment.base_, 0, static_cast<std::size_t>(length));
  return Result<SharedSegment>::success(std::move(segment));
}

MutableByteSpan StoredRegion::bytes() {
  if (shared) return MutableByteSpan(shared->data(), static_cast<std::size_t>(length));
  return MutableByteSpan(host.data(), host.size());
}

ByteSpan StoredRegion::bytes() const {
  if (shared) return ByteSpan(shared->data(), static_cast<std::size_t>(length));
  return ByteSpan(host.data(), host.size());
}

Result<RegionId> RegionStore::add_host_region(std::string name, std::uint64_t length,
                                              MemoryDomain domain, std::uint64_t seed,
                                              RegionId requested) {
  if (length == 0) {
    return Result<RegionId>::failure(
        Status(StatusCode::InvalidArgument, "region length must be greater than zero"));
  }
  StoredRegion region;
  region.id = requested.defined() ? requested : RegionId::from_value(next_id_++);
  if (requested.defined() && requested.value() >= next_id_) next_id_ = requested.value() + 1;
  if (regions_.count(region.id) != 0) {
    return Result<RegionId>::failure(Status(
        StatusCode::DuplicateIdentity, "the requested region identity is already present locally",
        region.id.to_string()));
  }
  region.name = std::move(name);
  region.memory_domain = domain;
  region.evidence_class = EvidenceClass::Real;
  region.backing = RegionBacking::HostHeap;
  region.length = length;
  region.host = make_pattern(length, seed);
  region.address_hint = reinterpret_cast<std::uint64_t>(region.host.data());
  const RegionId id = region.id;
  regions_.emplace(id, std::move(region));
  return Result<RegionId>::success(id);
}

Result<RegionId> RegionStore::add_shared_region(std::string name, const std::string& segment_name,
                                                std::uint64_t length, bool create) {
  auto segment = SharedSegment::open(segment_name, length, create);
  if (!segment.has_value()) return Result<RegionId>::failure(segment.status());
  StoredRegion region;
  region.id = RegionId::from_value(next_id_++);
  region.name = std::move(name);
  region.memory_domain = MemoryDomain::HostShared;
  region.evidence_class = EvidenceClass::Real;
  region.backing = RegionBacking::SharedMapping;
  region.length = length;
  region.shared = std::make_shared<SharedSegment>(std::move(segment.value()));
  region.address_hint = reinterpret_cast<std::uint64_t>(region.shared->data());
  const RegionId id = region.id;
  regions_.emplace(id, std::move(region));
  return Result<RegionId>::success(id);
}

Result<RegionId> RegionStore::add_synthetic_cxl_region(std::string name, std::uint64_t length,
                                                       std::uint64_t seed) {
  auto created = add_host_region(std::move(name), length, MemoryDomain::CxlClass, seed);
  if (!created.has_value()) return created;
  StoredRegion& region = regions_.at(created.value());
  // No physical CXL hardware is present, so the runtime must not claim real
  // evidence for this domain.
  region.evidence_class = EvidenceClass::Synthetic;
  region.backing = RegionBacking::Synthetic;
  return created;
}

Result<RegionId> RegionStore::attach_device_region(std::string name, std::string adapter,
                                                   MemoryDomain domain,
                                                   EvidenceClass evidence_class,
                                                   std::uint64_t length,
                                                   std::uint64_t address_hint) {
  StoredRegion region;
  region.id = RegionId::from_value(next_id_++);
  region.name = std::move(name);
  region.memory_domain = domain;
  region.evidence_class = evidence_class;
  region.backing = RegionBacking::DeviceLocal;
  region.length = length;
  region.address_hint = address_hint;
  region.device_adapter = std::move(adapter);
  const RegionId id = region.id;
  regions_.emplace(id, std::move(region));
  return Result<RegionId>::success(id);
}

StoredRegion* RegionStore::find(RegionId id) {
  const auto it = regions_.find(id);
  return it == regions_.end() ? nullptr : &it->second;
}

const StoredRegion* RegionStore::find(RegionId id) const {
  const auto it = regions_.find(id);
  return it == regions_.end() ? nullptr : &it->second;
}

StoredRegion& RegionStore::at(RegionId id) { return regions_.at(id); }
const StoredRegion& RegionStore::at(RegionId id) const { return regions_.at(id); }

std::vector<RegionId> RegionStore::ids() const {
  std::vector<RegionId> out;
  out.reserve(regions_.size());
  for (const auto& [id, region] : regions_) {
    (void)region;
    out.push_back(id);
  }
  return out;
}

Status RegionStore::fill(RegionId id, std::uint64_t seed, std::uint64_t offset,
                         std::uint64_t length) {
  StoredRegion* region = find(id);
  if (region == nullptr) {
    return Status(StatusCode::UnknownRegion, "no such stored region", id.to_string());
  }
  const std::uint64_t effective = length == 0 ? region->length : length;
  if (!checked_range(offset, effective, region->length)) {
    return Status(StatusCode::InvalidArgument, "fill range lies outside the region",
                  "offset=" + std::to_string(offset) + " length=" + std::to_string(effective));
  }
  MutableByteSpan view = region->bytes();
  DeterministicRandom random(seed == 0 ? 0x9E3779B97F4A7C15ull : seed);
  for (std::uint64_t i = 0; i < effective; ++i) {
    view[static_cast<std::size_t>(offset + i)] =
        static_cast<std::byte>(random.next_u32() & 0xFFu);
  }
  return Status::success();
}

Status RegionStore::poke(RegionId id, std::uint64_t offset, std::uint8_t value,
                         std::uint64_t count) {
  StoredRegion* region = find(id);
  if (region == nullptr) {
    return Status(StatusCode::UnknownRegion, "no such stored region", id.to_string());
  }
  if (!checked_range(offset, count, region->length)) {
    return Status(StatusCode::InvalidArgument, "poke range lies outside the region");
  }
  MutableByteSpan view = region->bytes();
  for (std::uint64_t i = 0; i < count; ++i) {
    view[static_cast<std::size_t>(offset + i)] = static_cast<std::byte>(value);
  }
  return Status::success();
}

Result<ContentFingerprint> RegionStore::fingerprint(RegionId id) const {
  const StoredRegion* region = find(id);
  if (region == nullptr) {
    return Result<ContentFingerprint>::failure(
        Status(StatusCode::UnknownRegion, "no such stored region", id.to_string()));
  }
  return Result<ContentFingerprint>::success(fingerprint_bytes(region->bytes()));
}

Status RegionStore::copy_and_verify(RegionId source, RegionId destination) {
  const StoredRegion* from = find(source);
  StoredRegion* to = find(destination);
  if (from == nullptr) {
    return Status(StatusCode::UnknownRegion, "no such source region", source.to_string());
  }
  if (to == nullptr) {
    return Status(StatusCode::UnknownRegion, "no such destination region",
                  destination.to_string());
  }
  if (from->length != to->length) {
    return Status(StatusCode::InvalidArgument, "source and destination lengths differ",
                  std::to_string(from->length) + " vs " + std::to_string(to->length));
  }
  const ByteSpan source_bytes = from->bytes();
  MutableByteSpan destination_bytes = to->bytes();
  // A shared mapping may alias the same pages; copying would then be a no-op
  // that silently "succeeds". Detect the alias and refuse rather than certify
  // a transfer that never happened.
  if (source_bytes.data() == destination_bytes.data()) {
    return Status(StatusCode::InvalidArgument,
                  "source and destination alias the same physical pages; a copy would not move "
                  "bytes and cannot be certified");
  }
  std::memcpy(destination_bytes.data(), source_bytes.data(),
              static_cast<std::size_t>(from->length));
  const ContentFingerprint expected = fingerprint_bytes(source_bytes);
  const ContentFingerprint observed = fingerprint_bytes(destination_bytes);
  if (!(expected == observed)) {
    return Status(StatusCode::ContentMismatch,
                  "the byte comparison after the copy did not reproduce the source contents");
  }
  return Status::success();
}

} // namespace adapters
} // namespace coherence
