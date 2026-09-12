// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/serialize.hpp"

namespace coherence {

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u16(std::uint16_t value) {
  buffer_.push_back(static_cast<std::byte>(value & 0xFFu));
  buffer_.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    buffer_.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    buffer_.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
  }
}

void ByteWriter::raw(ByteSpan bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }

void ByteWriter::text(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  const auto* ptr = reinterpret_cast<const std::byte*>(value.data());
  buffer_.insert(buffer_.end(), ptr, ptr + value.size());
}

void ByteWriter::blob(ByteSpan value) {
  u64(static_cast<std::uint64_t>(value.size()));
  raw(value);
}

void ByteWriter::uuid(UInt128 value) {
  u64(value.high);
  u64(value.low);
}

bool ByteReader::u8(std::uint8_t& out) {
  if (remaining() < 1) {
    failed_ = true;
    return false;
  }
  out = std::to_integer<std::uint8_t>(data_[offset_++]);
  return true;
}

bool ByteReader::u16(std::uint16_t& out) {
  if (remaining() < 2) {
    failed_ = true;
    return false;
  }
  out = static_cast<std::uint16_t>(load_u16(data_, offset_));
  offset_ += 2;
  return true;
}

bool ByteReader::u32(std::uint32_t& out) {
  if (remaining() < 4) {
    failed_ = true;
    return false;
  }
  out = load_u32(data_, offset_);
  offset_ += 4;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) {
  if (remaining() < 8) {
    failed_ = true;
    return false;
  }
  out = load_u64(data_, offset_);
  offset_ += 8;
  return true;
}

bool ByteReader::boolean(bool& out) {
  std::uint8_t raw = 0;
  if (!u8(raw)) return false;
  if (raw > 1) {
    failed_ = true;
    return false;
  }
  out = raw != 0;
  return true;
}

bool ByteReader::raw(MutableByteSpan out) {
  if (remaining() < out.size()) {
    failed_ = true;
    return false;
  }
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = data_[offset_ + i];
  offset_ += out.size();
  return true;
}

bool ByteReader::skip(std::size_t count) {
  if (remaining() < count) {
    failed_ = true;
    return false;
  }
  offset_ += count;
  return true;
}

bool ByteReader::text(std::string& out, std::uint32_t max_length) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (length > max_length) {
    failed_ = true;
    return false;
  }
  if (remaining() < length) {
    failed_ = true;
    return false;
  }
  out.assign(reinterpret_cast<const char*>(data_.data() + offset_), length);
  offset_ += length;
  return true;
}

bool ByteReader::blob(std::vector<std::byte>& out, std::uint64_t max_length) {
  std::uint64_t length = 0;
  if (!u64(length)) return false;
  if (length > max_length) {
    failed_ = true;
    return false;
  }
  if (remaining() < length) {
    failed_ = true;
    return false;
  }
  out.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
             data_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
  offset_ += static_cast<std::size_t>(length);
  return true;
}

bool ByteReader::uuid(UInt128& out) {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  if (!u64(high)) return false;
  if (!u64(low)) return false;
  out.high = high;
  out.low = low;
  return true;
}

bool ByteReader::digest(ContentDigest& out) {
  UInt128 value{};
  if (!uuid(value)) return false;
  out = ContentDigest::from_value(value);
  return true;
}

Status ByteReader::status() const {
  if (!failed_) return Status::success();
  return Status(StatusCode::TruncatedInput, "binary record ended before all fields were read",
                "offset=" + std::to_string(offset_) + " size=" + std::to_string(data_.size()));
}

Status ByteReader::expect_end(std::string_view what) const {
  if (failed_) return status();
  if (!at_end()) {
    return Status(StatusCode::TrailingGarbage, "unexpected trailing bytes after record",
                  std::string(what) + " trailing=" + std::to_string(remaining()));
  }
  return Status::success();
}

} // namespace coherence
