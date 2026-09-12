// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Byte-level primitives: overflow-checked arithmetic, little-endian codecs,
// CRC-32C and SHA-256 integrity primitives.
//
// Everything here is deterministic and allocation-bounded. The codecs never
// allocate on behalf of a peer-declared length before that length has been
// validated against a caller-supplied bound.
#ifndef COHERENCE_BYTES_HPP
#define COHERENCE_BYTES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coherence/export.hpp"

namespace coherence {

using ByteSpan = std::span<const std::byte>;
using MutableByteSpan = std::span<std::byte>;

// ---------------------------------------------------------------------------
// Overflow-checked arithmetic.
// ---------------------------------------------------------------------------

/// a + b, or std::nullopt on unsigned overflow.
[[nodiscard]] constexpr std::optional<std::uint64_t> checked_add(std::uint64_t a,
                                                                 std::uint64_t b) noexcept {
  if (a > UINT64_MAX - b) return std::nullopt;
  return a + b;
}

/// a * b, or std::nullopt on unsigned overflow.
[[nodiscard]] constexpr std::optional<std::uint64_t> checked_mul(std::uint64_t a,
                                                                 std::uint64_t b) noexcept {
  if (a != 0 && b > UINT64_MAX / a) return std::nullopt;
  return a * b;
}

/// Half-open range [offset, offset + length) validated against an extent.
[[nodiscard]] COHERENCE_API bool checked_range(std::uint64_t offset, std::uint64_t length,
                                               std::uint64_t extent) noexcept;

// ---------------------------------------------------------------------------
// Little-endian codecs. Fixed width, no varints, no host-endian dependence.
// ---------------------------------------------------------------------------
COHERENCE_API void store_u8(MutableByteSpan dst, std::size_t offset, std::uint8_t value);
COHERENCE_API void store_u16(MutableByteSpan dst, std::size_t offset, std::uint16_t value);
COHERENCE_API void store_u32(MutableByteSpan dst, std::size_t offset, std::uint32_t value);
COHERENCE_API void store_u64(MutableByteSpan dst, std::size_t offset, std::uint64_t value);

[[nodiscard]] COHERENCE_API std::uint8_t load_u8(ByteSpan src, std::size_t offset);
[[nodiscard]] COHERENCE_API std::uint16_t load_u16(ByteSpan src, std::size_t offset);
[[nodiscard]] COHERENCE_API std::uint32_t load_u32(ByteSpan src, std::size_t offset);
[[nodiscard]] COHERENCE_API std::uint64_t load_u64(ByteSpan src, std::size_t offset);

// ---------------------------------------------------------------------------
// Integrity.
// ---------------------------------------------------------------------------

/// CRC-32C (Castagnoli, reflected polynomial 0x1EDC6F41 / 0x82F63B78), the
/// same polynomial used by iSCSI, ext4 metadata checksums and SCTP.
[[nodiscard]] COHERENCE_API std::uint32_t crc32c(ByteSpan data) noexcept;
[[nodiscard]] COHERENCE_API std::uint32_t crc32c(std::uint32_t seed, ByteSpan data) noexcept;

/// SHA-256 (FIPS 180-4). Used for whole-file integrity where CRC-32C alone is
/// not strong enough to distinguish a corrupt file from a deliberately
/// manipulated one.
class COHERENCE_API Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;

  Sha256() noexcept;
  void update(ByteSpan data) noexcept;
  void update(std::string_view text) noexcept;
  [[nodiscard]] std::array<std::byte, kDigestBytes> finish() noexcept;

  [[nodiscard]] static std::array<std::byte, kDigestBytes> digest(ByteSpan data) noexcept;

 private:
  void compress(const std::byte* block) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_length_;
  std::array<std::byte, 64> buffer_;
  std::size_t buffered_;
};

[[nodiscard]] COHERENCE_API std::string to_hex(ByteSpan data);
[[nodiscard]] COHERENCE_API std::string sha256_hex(ByteSpan data);

/// Constant-time equality for digests.
[[nodiscard]] COHERENCE_API bool digest_equal(ByteSpan a, ByteSpan b) noexcept;

// ---------------------------------------------------------------------------
// Deterministic pseudo-random generator (xoshiro256**). Seeded explicitly so
// that randomized property tests are reproducible and record their seed.
// ---------------------------------------------------------------------------
class COHERENCE_API DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed) noexcept;
  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t next_u32() noexcept;
  /// Uniform value in [0, bound). bound must be non-zero.
  [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) noexcept;
  [[nodiscard]] bool next_bool() noexcept { return (next_u64() & 1u) != 0; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_[4];
  std::uint64_t seed_;
};

} // namespace coherence

#endif // COHERENCE_BYTES_HPP
