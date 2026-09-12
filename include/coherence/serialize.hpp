// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Deterministic, bounds-checked binary codecs.
//
// Properties relied upon elsewhere:
//  * Fixed-width little-endian encoding. No varints, no host-endian
//    dependence, no padding, no pointer or address values.
//  * Canonical field order per record type, so equivalent canonical state
//    always produces byte-identical output.
//  * A reader never allocates on behalf of a peer-declared length before the
//    length has been validated against a caller-supplied bound and against the
//    remaining input.
#ifndef COHERENCE_SERIALIZE_HPP
#define COHERENCE_SERIALIZE_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/status.hpp"

namespace coherence {

class COHERENCE_API ByteWriter {
 public:
  ByteWriter() = default;
  explicit ByteWriter(std::size_t reserve_bytes) { buffer_.reserve(reserve_bytes); }

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void raw(ByteSpan bytes);

  /// Length-prefixed (u32) string. Bounded to UINT32_MAX bytes.
  void text(std::string_view value);
  /// Length-prefixed (u64) byte string.
  void blob(ByteSpan value);

  template <typename Tag, typename Rep>
  void strong_id(StrongId<Tag, Rep> value) {
    if constexpr (requires(Rep r) { r.high; r.low; }) {
      uuid(value.value());
    } else {
      u64(static_cast<std::uint64_t>(value.value()));
    }
  }

  void uuid(UInt128 value);
  void digest(ContentDigest value) { uuid(value.value()); }

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] ByteSpan span() const noexcept { return ByteSpan(buffer_); }
  void clear() noexcept { buffer_.clear(); }

 private:
  std::vector<std::byte> buffer_;
};

class COHERENCE_API ByteReader {
 public:
  explicit ByteReader(ByteSpan data) : data_(data) {}

  [[nodiscard]] bool u8(std::uint8_t& out);
  [[nodiscard]] bool u16(std::uint16_t& out);
  [[nodiscard]] bool u32(std::uint32_t& out);
  [[nodiscard]] bool u64(std::uint64_t& out);
  [[nodiscard]] bool boolean(bool& out);
  [[nodiscard]] bool raw(MutableByteSpan out);
  [[nodiscard]] bool skip(std::size_t count);
  [[nodiscard]] bool text(std::string& out, std::uint32_t max_length);
  [[nodiscard]] bool blob(std::vector<std::byte>& out, std::uint64_t max_length);

  template <typename Tag, typename Rep>
  [[nodiscard]] bool strong_id(StrongId<Tag, Rep>& out) {
    if constexpr (requires(Rep r) { r.high; r.low; }) {
      UInt128 value{};
      if (!uuid(value)) return false;
      out = StrongId<Tag, Rep>::from_value(value);
      return true;
    } else {
      std::uint64_t raw_value = 0;
      if (!u64(raw_value)) return false;
      out = StrongId<Tag, Rep>::from_value(static_cast<Rep>(raw_value));
      return true;
    }
  }

  [[nodiscard]] bool uuid(UInt128& out);
  [[nodiscard]] bool digest(ContentDigest& out);

  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] Status status() const;

  /// Validate that no bytes remain. Trailing bytes are a protocol/persistence
  /// defect, never silently ignored.
  [[nodiscard]] Status expect_end(std::string_view what) const;

 private:
  ByteSpan data_;
  std::size_t offset_ = 0;
  bool failed_ = false;
};

/// Round-trip friendly helper used by tests: encode with a function, then
/// decode with the mirrored function, and require full consumption.
template <typename T, typename Encode, typename Decode>
Status round_trip(const T& value, Encode encode, Decode decode, T& out) {
  ByteWriter writer;
  encode(writer, value);
  ByteReader reader(writer.span());
  if (!decode(reader, out)) return reader.status();
  return reader.expect_end("record");
}

} // namespace coherence

#endif // COHERENCE_SERIALIZE_HPP
