// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/ids.hpp"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace coherence {
namespace {

/// Fill the buffer from the operating system cryptographic random source.
/// Returns false if no such source is available; callers must not fall back to
/// a guessable value for boot or session identity.
bool secure_random_bytes(std::span<std::byte> out) noexcept {
  if (out.empty()) return true;
#if defined(_WIN32)
  NTSTATUS status = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()),
                                      static_cast<ULONG>(out.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return status == 0;  // STATUS_SUCCESS == 0
#else
  std::size_t filled = 0;
  int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  while (filled < out.size()) {
    const ssize_t n = ::read(fd, out.data() + filled, out.size() - filled);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    filled += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return true;
#endif
}

} // namespace

std::string to_hex(UInt128 value) {
  char buffer[33];
  std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                static_cast<unsigned long long>(value.high),
                static_cast<unsigned long long>(value.low));
  return std::string(buffer, 32);
}

std::string UInt128::to_string() const { return to_hex(*this); }

std::string format_id(std::string_view kind, std::uint64_t value) {
  std::string out(kind);
  out.push_back('#');
  out.append(std::to_string(value));
  return out;
}

std::string format_id(std::string_view kind, UInt128 value) {
  std::string out(kind);
  out.push_back('#');
  out.append(to_hex(value));
  return out;
}

ParticipantBootId generate_participant_boot_id() {
  UInt128 raw{};
  if (!secure_random_bytes(std::span<std::byte>(reinterpret_cast<std::byte*>(&raw), sizeof(raw)))) {
    // A guessable boot identity would allow a stale process to regain
    // authority. Refuse to invent one: the runtime treats a nil identity as
    // unusable and every authority-bearing path rejects it.
    return ParticipantBootId::nil();
  }
  if (raw.is_zero()) raw.low = 1;
  return ParticipantBootId::from_value(raw);
}

SessionId generate_session_id() {
  UInt128 raw{};
  if (!secure_random_bytes(std::span<std::byte>(reinterpret_cast<std::byte*>(&raw), sizeof(raw)))) {
    return SessionId::nil();
  }
  if (raw.is_zero()) raw.low = 1;
  return SessionId::from_value(raw);
}

RequestId generate_request_id() {
  std::uint64_t raw = 0;
  if (!secure_random_bytes(std::span<std::byte>(reinterpret_cast<std::byte*>(&raw), sizeof(raw)))) {
    return RequestId::nil();
  }
  if (raw == 0) raw = 1;
  return RequestId::from_value(raw);
}

} // namespace coherence
