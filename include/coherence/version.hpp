// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#ifndef COHERENCE_VERSION_HPP
#define COHERENCE_VERSION_HPP

#include <cstdint>
#include <string>

#include "coherence/export.hpp"

#define COHERENCE_FABRIC_VERSION_MAJOR 1
#define COHERENCE_FABRIC_VERSION_MINOR 0
#define COHERENCE_FABRIC_VERSION_PATCH 0

// Monotonic wire/persistence compatibility level. Incremented whenever the
// framed control-plane protocol or the persistence schema changes in a way that
// is not backward compatible.
#define COHERENCE_FABRIC_ABI_LEVEL 1

namespace coherence {

struct Version {
  std::uint32_t major = COHERENCE_FABRIC_VERSION_MAJOR;
  std::uint32_t minor = COHERENCE_FABRIC_VERSION_MINOR;
  std::uint32_t patch = COHERENCE_FABRIC_VERSION_PATCH;

  friend constexpr bool operator==(const Version&, const Version&) noexcept = default;
};

/// Version of the linked library.
COHERENCE_API Version library_version() noexcept;

/// Deterministic dotted version string, e.g. "1.0.0".
COHERENCE_API std::string version_string();

/// Control-plane protocol version implemented by this build.
COHERENCE_API std::uint32_t protocol_version() noexcept;

/// Persistence schema identifier implemented by this build.
COHERENCE_API std::uint32_t persistence_schema() noexcept;

/// Short build identification string (compiler / configuration). The value is
/// deterministic for a given build and is reported by the CLI and by the
/// coordinator handshake.
COHERENCE_API const char* build_identification() noexcept;

} // namespace coherence

#endif // COHERENCE_VERSION_HPP
