// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Coherence policy.
//
// Policy is a first-class, versioned object. It is never stored only in
// process-global configuration, and every authority-bearing decision records
// the PolicyId and PolicyGeneration under which it was taken, so that a policy
// change makes earlier derived decisions stale.
//
// A policy whose semantics are not implemented is rejected at validation time
// rather than degraded at run time.
#ifndef COHERENCE_POLICY_HPP
#define COHERENCE_POLICY_HPP

#include <cstdint>
#include <string>

#include "coherence/enums.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/status.hpp"

namespace coherence {

inline constexpr std::uint64_t kUnboundedStaleness = UINT64_MAX;

struct COHERENCE_API CoherencePolicy {
  PolicyId id;
  PolicyGeneration generation;
  std::string name;

  ConsistencyModel consistency = ConsistencyModel::Unspecified;
  WriteOwnershipMode write_ownership = WriteOwnershipMode::Unspecified;
  PublicationDurability publication_durability = PublicationDurability::Unspecified;
  ConflictBehavior conflict_behavior = ConflictBehavior::Unspecified;
  RecoveryPolicy recovery_policy = RecoveryPolicy::Unspecified;
  StaleReadPolicy stale_read_policy = StaleReadPolicy::Unspecified;
  DirtyLossPolicy dirty_loss_policy = DirtyLossPolicy::Unspecified;

  /// Maximum number of published versions a stale replica may trail the
  /// authoritative version while still serving a stale-allowed read. Only
  /// meaningful when stale_read_policy == Bounded.
  std::uint64_t stale_read_bound_versions = 0;

  /// Maximum age of an evidence observation, counted in coordinator logical
  /// operations. Never wall-clock time, never scheduling order.
  ///
  /// Zero means evidence is invalidated only by events: a coordinator epoch
  /// advance, a participant boot change, an ownership change, a publication
  /// that supersedes the replica, an invalidation, or retirement. A non-zero
  /// value additionally expires evidence once the coordinator has performed
  /// that many logical operations since the observation.
  std::uint64_t evidence_max_age_operations = 0;

  /// When true, a publication may not commit until every mandatory
  /// invalidation has been acknowledged. When false the publication commits on
  /// durability and outstanding invalidations are tracked separately.
  bool require_invalidation_acks_before_publication = true;

  /// When true, a mandatory synchronization must complete before a replica may
  /// be treated as current again.
  bool require_sync_completion_for_currentness = true;

  /// Bit mask over MemoryDomain values. A region whose domain is not permitted
  /// by the policy cannot join a coherence object governed by it.
  std::uint32_t allowed_domains_mask = 0xFFFFFFFFu;

  /// When true, a publication must include a content fingerprint produced from
  /// real bytes. Fingerprints are what let the runtime verify that a
  /// synchronization actually moved the bytes it claims to have moved.
  bool require_content_fingerprint = true;

  [[nodiscard]] bool allows_domain(MemoryDomain domain) const noexcept {
    return (allowed_domains_mask & (1u << static_cast<std::uint32_t>(domain))) != 0u;
  }
};

/// Validate a policy. Returns Unsupported for semantics that are declared but
/// not implemented, and InvalidArgument for contradictory combinations.
[[nodiscard]] COHERENCE_API Status validate_policy(const CoherencePolicy& policy);

/// Construct the canonical policy for a consistency model. The returned policy
/// has generation 1 and id 0; the coordinator assigns identities on
/// registration.
[[nodiscard]] COHERENCE_API CoherencePolicy make_policy(ConsistencyModel model,
                                                        std::string name = std::string());

/// Policy with conservative single-writer authority, durable publication,
/// mandatory invalidation acknowledgements and strict fail-closed reads.
[[nodiscard]] COHERENCE_API CoherencePolicy strict_policy(std::string name = "strict");

/// Strict reads, but publication commits once every mandatory invalidation has
/// been acknowledged; readers must acquire at or beyond the published version.
[[nodiscard]] COHERENCE_API CoherencePolicy release_acquire_policy(
    std::string name = "release_acquire");

/// Readers bind to an explicit snapshot version.
[[nodiscard]] COHERENCE_API CoherencePolicy snapshot_policy(std::string name = "snapshot");

/// Stale reads permitted up to a bound. Latest-known and authoritative versions
/// are always reported separately.
[[nodiscard]] COHERENCE_API CoherencePolicy eventual_policy(std::uint64_t stale_bound,
                                                            std::string name = "eventual");

/// Deterministic, stable multi-line rendering used by the CLI and by tests.
[[nodiscard]] COHERENCE_API std::string render_policy(const CoherencePolicy& policy);

/// Compact one-line form: "name/gen consistency=... ownership=...".
[[nodiscard]] COHERENCE_API std::string summarize_policy(const CoherencePolicy& policy);

} // namespace coherence

#endif // COHERENCE_POLICY_HPP
