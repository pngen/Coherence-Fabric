// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Strongly typed identities.
//
// Coherence Fabric distinguishes several *independent* authority domains:
// a coordinator epoch, an object generation, a region generation, a replica
// generation, an ownership generation and a policy generation are not
// interchangeable integers. They are modelled as distinct C++ types so that a
// generation from one authority domain cannot be silently substituted for
// another, and so that a stale identity can never regain authority merely
// because an integer, address, process id or connection id was reused.
#ifndef COHERENCE_IDS_HPP
#define COHERENCE_IDS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "coherence/export.hpp"

namespace coherence {

/// 128-bit identity used where reuse would be catastrophic (process boot
/// identity, session identity). Generated from a cryptographic source and
/// therefore never intentionally recycled across process lifetimes.
struct UInt128 {
  std::uint64_t high = 0;
  std::uint64_t low = 0;

  friend constexpr bool operator==(const UInt128&, const UInt128&) noexcept = default;

  [[nodiscard]] constexpr bool is_zero() const noexcept { return high == 0 && low == 0; }

  /// Deterministic 32-character lowercase hex rendering.
  [[nodiscard]] std::string to_string() const;
};

COHERENCE_API std::string to_hex(UInt128 value);

template <typename Tag, typename Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep raw) noexcept : value_(raw) {}

  /// Explicitly named constructor: reading call sites should not look like an
  /// integer conversion.
  [[nodiscard]] static constexpr StrongId from_value(Rep raw) noexcept { return StrongId(raw); }
  [[nodiscard]] static constexpr StrongId nil() noexcept { return StrongId(Rep{}); }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool defined() const noexcept { return !is_nil(); }
  [[nodiscard]] constexpr bool is_nil() const noexcept {
    if constexpr (requires(Rep r) { r.is_zero(); }) {
      return value_.is_zero();
    } else {
      return value_ == Rep{};
    }
  }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept = default;

  friend constexpr bool operator<(StrongId a, StrongId b) noexcept {
    if constexpr (requires(Rep r) { r.high; r.low; }) {
      if (a.value_.high != b.value_.high) return a.value_.high < b.value_.high;
      return a.value_.low < b.value_.low;
    } else {
      return a.value_ < b.value_;
    }
  }
  friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return b < a; }
  friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(StrongId a, StrongId b) noexcept { return !(a < b); }
  friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return !(a == b); }

  /// Successor in the same authority domain. Used for generation progression;
  /// never derived from wall-clock time or scheduling order.
  [[nodiscard]] constexpr StrongId next() const noexcept {
    if constexpr (requires(Rep r) { r.high; r.low; }) {
      Rep out = value_;
      out.low += 1;
      if (out.low == 0) out.high += 1;
      return StrongId(out);
    } else {
      return StrongId(static_cast<Rep>(value_ + 1));
    }
  }

  [[nodiscard]] std::string to_string() const {
    if constexpr (requires(Rep r) { r.high; r.low; }) {
      return to_hex(value_);
    } else {
      return std::to_string(static_cast<std::uint64_t>(value_));
    }
  }

 private:
  Rep value_{};
};

// ---------------------------------------------------------------------------
// Identity tags. Each tag creates a genuinely distinct C++ type.
// ---------------------------------------------------------------------------
#define COHERENCE_DEFINE_ID(name, tag)                       \
  struct tag {};                                             \
  using name = StrongId<tag>;                                \
  struct tag##Hasher {                                       \
    std::size_t operator()(name id) const noexcept {         \
      return std::hash<std::uint64_t>{}(                     \
          static_cast<std::uint64_t>(id.value()));           \
    }                                                        \
  }

#define COHERENCE_DEFINE_UUID(name, tag)                     \
  struct tag {};                                             \
  using name = StrongId<tag, UInt128>;                       \
  struct tag##Hasher {                                       \
    std::size_t operator()(name id) const noexcept {         \
      return std::hash<std::uint64_t>{}(                     \
          id.value().high ^ (id.value().low * 0x9E3779B97F4A7C15ull)); \
    }                                                        \
  }

COHERENCE_DEFINE_ID(CoherenceDomainId, CoherenceDomainIdTag);
COHERENCE_DEFINE_ID(ObjectId, ObjectIdTag);
COHERENCE_DEFINE_ID(ObjectGeneration, ObjectGenerationTag);
COHERENCE_DEFINE_ID(RegionId, RegionIdTag);
COHERENCE_DEFINE_ID(RegionGeneration, RegionGenerationTag);
COHERENCE_DEFINE_ID(ReplicaId, ReplicaIdTag);
COHERENCE_DEFINE_ID(ReplicaGeneration, ReplicaGenerationTag);
COHERENCE_DEFINE_ID(ParticipantId, ParticipantIdTag);
COHERENCE_DEFINE_ID(CoordinatorEpoch, CoordinatorEpochTag);
COHERENCE_DEFINE_ID(OwnershipGeneration, OwnershipGenerationTag);
COHERENCE_DEFINE_ID(VersionId, VersionIdTag);
COHERENCE_DEFINE_ID(PublicationId, PublicationIdTag);
COHERENCE_DEFINE_ID(InvalidationId, InvalidationIdTag);
COHERENCE_DEFINE_ID(SyncOperationId, SyncOperationIdTag);
COHERENCE_DEFINE_ID(LeaseId, LeaseIdTag);
COHERENCE_DEFINE_ID(PolicyId, PolicyIdTag);
COHERENCE_DEFINE_ID(PolicyGeneration, PolicyGenerationTag);
COHERENCE_DEFINE_ID(EvidenceId, EvidenceIdTag);
COHERENCE_DEFINE_ID(EvidenceGeneration, EvidenceGenerationTag);
COHERENCE_DEFINE_ID(DecisionId, DecisionIdTag);
COHERENCE_DEFINE_ID(RequestId, RequestIdTag);
COHERENCE_DEFINE_ID(OperationSequence, OperationSequenceTag);

// Identity whose reuse across process lifetimes is dangerous.
COHERENCE_DEFINE_UUID(ParticipantBootId, ParticipantBootIdTag);
COHERENCE_DEFINE_UUID(SessionId, SessionIdTag);
COHERENCE_DEFINE_UUID(ContentDigest, ContentDigestTag);

#undef COHERENCE_DEFINE_ID
#undef COHERENCE_DEFINE_UUID

/// Human-readable label for an identity *kind*, used by diagnostics so that a
/// rendering never hides which authority domain a value belongs to.
COHERENCE_API std::string format_id(std::string_view kind, std::uint64_t value);
COHERENCE_API std::string format_id(std::string_view kind, UInt128 value);

/// Generate a process-unique boot identity. The value is drawn from the
/// operating system cryptographic random source and is never derived from a
/// process id, thread id, address, or timestamp.
COHERENCE_API ParticipantBootId generate_participant_boot_id();

/// Generate a session identity for a control-plane connection.
COHERENCE_API SessionId generate_session_id();

/// Generate a fresh request identity (client-side). Used for idempotent retry.
COHERENCE_API RequestId generate_request_id();

} // namespace coherence

namespace std {
template <typename Tag, typename Rep>
struct hash<coherence::StrongId<Tag, Rep>> {
  std::size_t operator()(coherence::StrongId<Tag, Rep> id) const noexcept {
    if constexpr (requires(Rep r) { r.high; r.low; }) {
      return std::hash<std::uint64_t>{}(
          id.value().high ^ (id.value().low * 0x9E3779B97F4A7C15ull));
    } else {
      return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(id.value()));
    }
  }
};

template <>
struct hash<coherence::UInt128> {
  std::size_t operator()(coherence::UInt128 v) const noexcept {
    return std::hash<std::uint64_t>{}(v.high ^ (v.low * 0x9E3779B97F4A7C15ull));
  }
};
} // namespace std

#endif // COHERENCE_IDS_HPP
