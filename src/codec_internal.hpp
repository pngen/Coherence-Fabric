// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
// Internal helpers shared by the codec translation units. Not installed.
#ifndef COHERENCE_SRC_CODEC_INTERNAL_HPP
#define COHERENCE_SRC_CODEC_INTERNAL_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "coherence/codec.hpp"

namespace coherence {
namespace codec_detail {

template <typename Enum>
void enc_enum(ByteWriter& w, Enum value) {
  w.u8(static_cast<std::uint8_t>(value));
}

template <typename Enum, typename Parse>
bool dec_enum(ByteReader& r, Enum& out, Parse parse) {
  std::uint8_t raw = 0;
  if (!r.u8(raw)) return false;
  const auto parsed = parse(raw);
  if (!parsed.has_value()) return false;
  out = *parsed;
  return true;
}

inline bool dec_text(ByteReader& r, std::string& out, const DecodeLimits& limits) {
  return r.text(out, limits.max_text);
}

inline bool dec_header(ByteReader& r, std::uint16_t expected) {
  std::uint16_t version = 0;
  if (!r.u16(version)) return false;
  return version == expected;
}

inline bool dec_code(ByteReader& r, StatusCode& out) {
  std::uint16_t raw = 0;
  if (!r.u16(raw)) return false;
  out = static_cast<StatusCode>(raw);
  return status_code_name(out) != std::string_view{"unknown_status"};
}

inline void enc_code(ByteWriter& w, StatusCode code) {
  w.u16(static_cast<std::uint16_t>(code));
}

} // namespace codec_detail
} // namespace coherence

#endif // COHERENCE_SRC_CODEC_INTERNAL_HPP
