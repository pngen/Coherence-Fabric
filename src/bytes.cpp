// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/bytes.hpp"

#include <cstring>

namespace coherence {
namespace {

// --- CRC-32C ---------------------------------------------------------------
struct Crc32cTable {
  std::uint32_t entries[8][256];

  constexpr Crc32cTable() noexcept : entries{} {
    constexpr std::uint32_t kPoly = 0x82F63B78u;  // reflected Castagnoli
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1) ^ kPoly : (crc >> 1);
      }
      entries[0][i] = crc;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = entries[0][i];
      for (std::size_t slice = 1; slice < 8; ++slice) {
        crc = entries[0][crc & 0xFFu] ^ (crc >> 8);
        entries[slice][i] = crc;
      }
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};

// --- SHA-256 ---------------------------------------------------------------
constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
  return (x >> n) | (x << (32u - n));
}

[[nodiscard]] constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
[[nodiscard]] constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
[[nodiscard]] constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}
[[nodiscard]] constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

} // namespace

bool checked_range(std::uint64_t offset, std::uint64_t length, std::uint64_t extent) noexcept {
  const auto end = checked_add(offset, length);
  if (!end.has_value()) return false;
  return *end <= extent;
}

void store_u8(MutableByteSpan dst, std::size_t offset, std::uint8_t value) {
  dst[offset] = static_cast<std::byte>(value);
}
void store_u16(MutableByteSpan dst, std::size_t offset, std::uint16_t value) {
  dst[offset] = static_cast<std::byte>(value & 0xFFu);
  dst[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}
void store_u32(MutableByteSpan dst, std::size_t offset, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    dst[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
  }
}
void store_u64(MutableByteSpan dst, std::size_t offset, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    dst[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
  }
}

std::uint8_t load_u8(ByteSpan src, std::size_t offset) {
  return std::to_integer<std::uint8_t>(src[offset]);
}
std::uint16_t load_u16(ByteSpan src, std::size_t offset) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(src[offset])) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(src[offset + 1]))
                                    << 8);
}
std::uint32_t load_u32(ByteSpan src, std::size_t offset) {
  std::uint32_t out = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(src[offset + i])) << (8 * i);
  }
  return out;
}
std::uint64_t load_u64(ByteSpan src, std::size_t offset) {
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    out |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[offset + i])) << (8 * i);
  }
  return out;
}

std::uint32_t crc32c(std::uint32_t seed, ByteSpan data) noexcept {
  std::uint32_t crc = ~seed;
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t remaining = data.size();

  while (remaining >= 8) {
    std::uint64_t word = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      word |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    crc ^= static_cast<std::uint32_t>(word & 0xFFFFFFFFu);
    crc = kCrc32cTable.entries[7][crc & 0xFFu] ^
          kCrc32cTable.entries[6][(crc >> 8) & 0xFFu] ^
          kCrc32cTable.entries[5][(crc >> 16) & 0xFFu] ^
          kCrc32cTable.entries[4][(crc >> 24) & 0xFFu] ^
          kCrc32cTable.entries[3][static_cast<std::uint32_t>(word >> 32) & 0xFFu] ^
          kCrc32cTable.entries[2][(static_cast<std::uint32_t>(word >> 32) >> 8) & 0xFFu] ^
          kCrc32cTable.entries[1][(static_cast<std::uint32_t>(word >> 32) >> 16) & 0xFFu] ^
          kCrc32cTable.entries[0][(static_cast<std::uint32_t>(word >> 32) >> 24) & 0xFFu];
    p += 8;
    remaining -= 8;
  }
  while (remaining > 0) {
    crc = kCrc32cTable.entries[0][(crc ^ *p) & 0xFFu] ^ (crc >> 8);
    ++p;
    --remaining;
  }
  return ~crc;
}

std::uint32_t crc32c(ByteSpan data) noexcept { return crc32c(0u, data); }

// --- SHA-256 ---------------------------------------------------------------

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
      bit_length_(0),
      buffer_{},
      buffered_(0) {}

void Sha256::compress(const std::byte* block) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[i * 4])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[i * 4 + 1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[i * 4 + 2])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[i * 4 + 3])));
  }
  for (std::size_t i = 16; i < 64; ++i) {
    w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + big_sigma1(e) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
    const std::uint32_t t2 = big_sigma0(a) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(ByteSpan data) noexcept {
  bit_length_ += static_cast<std::uint64_t>(data.size()) * 8u;
  std::size_t offset = 0;
  if (buffered_ != 0) {
    while (buffered_ < 64 && offset < data.size()) {
      buffer_[buffered_++] = data[offset++];
    }
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (data.size() - offset >= 64) {
    compress(data.data() + offset);
    offset += 64;
  }
  while (offset < data.size()) {
    buffer_[buffered_++] = data[offset++];
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::array<std::byte, Sha256::kDigestBytes> Sha256::finish() noexcept {
  const std::uint64_t total_bits = bit_length_;
  buffer_[buffered_++] = std::byte{0x80};
  if (buffered_ > 56) {
    while (buffered_ < 64) buffer_[buffered_++] = std::byte{0};
    compress(buffer_.data());
    buffered_ = 0;
  }
  while (buffered_ < 56) buffer_[buffered_++] = std::byte{0};
  for (std::size_t i = 0; i < 8; ++i) {
    buffer_[56 + i] = static_cast<std::byte>((total_bits >> (8 * (7 - i))) & 0xFFu);
  }
  compress(buffer_.data());

  std::array<std::byte, kDigestBytes> digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::byte>((state_[i] >> 24) & 0xFFu);
    digest[i * 4 + 1] = static_cast<std::byte>((state_[i] >> 16) & 0xFFu);
    digest[i * 4 + 2] = static_cast<std::byte>((state_[i] >> 8) & 0xFFu);
    digest[i * 4 + 3] = static_cast<std::byte>(state_[i] & 0xFFu);
  }
  return digest;
}

std::array<std::byte, Sha256::kDigestBytes> Sha256::digest(ByteSpan data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

std::string to_hex(ByteSpan data) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const std::byte b : data) {
    const auto v = std::to_integer<std::uint8_t>(b);
    out.push_back(kHex[(v >> 4) & 0x0Fu]);
    out.push_back(kHex[v & 0x0Fu]);
  }
  return out;
}

std::string sha256_hex(ByteSpan data) { return to_hex(Sha256::digest(data)); }

bool digest_equal(ByteSpan a, ByteSpan b) noexcept {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(std::to_integer<std::uint8_t>(a[i]) ^
                                       std::to_integer<std::uint8_t>(b[i]));
  }
  return diff == 0;
}

// --- DeterministicRandom ---------------------------------------------------

DeterministicRandom::DeterministicRandom(std::uint64_t seed) noexcept : seed_(seed) {
  // splitmix64 expansion so that adjacent seeds produce unrelated streams.
  std::uint64_t x = seed;
  for (std::size_t i = 0; i < 4; ++i) {
    x += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    state_[i] = z;
  }
  if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) {
    state_[0] = 0x9E3779B97F4A7C15ull;
  }
}

std::uint64_t DeterministicRandom::next_u64() noexcept {
  const std::uint64_t result = ((state_[1] * 5u) << 7) | ((state_[1] * 5u) >> 57);
  const std::uint64_t t = state_[1] << 17;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = (state_[3] << 45) | (state_[3] >> 19);
  return result;
}

std::uint32_t DeterministicRandom::next_u32() noexcept {
  return static_cast<std::uint32_t>(next_u64() >> 32);
}

std::uint64_t DeterministicRandom::next_below(std::uint64_t bound) noexcept {
  if (bound <= 1) return 0;
  // Lemire's multiply-shift rejection method: unbiased and deterministic.
  const std::uint64_t threshold = (~bound + 1u) % bound;
  for (;;) {
    const std::uint64_t r = next_u64();
    if (r >= threshold) return r % bound;
  }
}

} // namespace coherence
