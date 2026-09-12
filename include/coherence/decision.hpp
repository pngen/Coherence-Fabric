// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Structured decisions.
//
// A decision is never a bare boolean. It carries the identities, generations,
// versions, evidence and rationale that produced it, so that every important
// coherence outcome can be explained after the fact and re-derived
// deterministically from the same canonical state and policy.
#ifndef COHERENCE_DECISION_HPP
#define COHERENCE_DECISION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/enums.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/model.hpp"
#include "coherence/status.hpp"

namespace coherence {

/// Generations that were consulted while producing a decision.
struct COHERENCE_API DecisionContext {
  DecisionId decision;
  CoordinatorEpoch epoch;
  PolicyId policy;
  PolicyGeneration policy_generation;
  ObjectId object;
  ObjectGeneration object_generation;
  OwnershipGeneration ownership_generation;
  OperationSequence sequence;
};

struct COHERENCE_API ReadDecision {
  DecisionContext context;
  ReadOutcome outcome = ReadOutcome::Unknown;
  StatusCode reason = StatusCode::UnknownObject;

  RegionId region;
  RegionGeneration region_generation;
  CoherenceState region_state = CoherenceState::Unknown;

  VersionId authoritative_version;
  VersionId region_version;
  /// authoritative_version - region_version when both are known and comparable.
  std::uint64_t staleness = 0;
  bool staleness_known = false;

  bool sync_required = false;
  bool stale_allowed = false;
  bool evidence_fresh = false;

  EvidenceId evidence;
  EvidenceGeneration evidence_generation;
  EvidenceClass evidence_class = EvidenceClass::Unsupported;

  SyncOperationId required_sync;
  LeaseId lease;

  /// Human-readable, deterministic rationale. Never empty.
  std::string rationale;
};

struct COHERENCE_API WriteGrant {
  DecisionContext context;
  bool granted = false;
  StatusCode reason = StatusCode::UnknownObject;

  ParticipantId writer;
  ParticipantBootId writer_boot;

  VersionId base_version;
  VersionId authoritative_version;

  RegionId writer_region;
  RegionGeneration writer_region_generation;

  /// Invalidations that must be acknowledged before the writer may mutate.
  std::vector<InvalidationRecord> required_invalidations;

  /// True when the writer may mutate immediately (no outstanding work).
  bool may_mutate_now = false;

  std::string rationale;
};

struct COHERENCE_API PublicationReceipt {
  DecisionContext context;
  PublicationId publication;
  PublicationState state = PublicationState::None;
  StatusCode reason = StatusCode::Ok;

  VersionId version;
  VersionId prior_version;
  ParticipantId writer;
  ParticipantBootId writer_boot;

  ContentFingerprint content;
  PublicationDurability durability = PublicationDurability::Unspecified;
  bool durable = false;
  /// True when this receipt is the replay of an already committed publication
  /// carrying the same RequestId. A retried request never produces a second
  /// independent version.
  bool idempotent_replay = false;
  /// True when the publication was completed by recovery rather than by the
  /// original writer.
  bool completed_by_recovery = false;

  std::string rationale;
};

struct COHERENCE_API InvalidationOutcome {
  DecisionContext context;
  InvalidationId invalidation;
  InvalidationState state = InvalidationState::Unknown;
  StatusCode reason = StatusCode::Ok;
  RegionId region;
  RegionGeneration region_generation;
  ReplicaGeneration replica_generation;
  bool idempotent_replay = false;
  std::string rationale;
};

struct COHERENCE_API SyncOutcome {
  DecisionContext context;
  SyncOperationId operation;
  SyncState state = SyncState::Planned;
  StatusCode reason = StatusCode::Ok;
  SyncRecord plan;
  bool idempotent_replay = false;
  std::string rationale;
};

struct COHERENCE_API ReleaseOutcome {
  DecisionContext context;
  StatusCode reason = StatusCode::Ok;
  LeaseId lease;
  ParticipantId participant;
  ParticipantBootId boot;
  VersionId version;
  bool already_released = false;
  std::string rationale;
};

// ---------------------------------------------------------------------------
// Deterministic renderings.
// ---------------------------------------------------------------------------
COHERENCE_API std::string render_read_decision(const ReadDecision& decision);
COHERENCE_API std::string render_write_grant(const WriteGrant& grant);
COHERENCE_API std::string render_publication(const PublicationReceipt& receipt);
COHERENCE_API std::string render_invalidation_outcome(const InvalidationOutcome& outcome);
COHERENCE_API std::string render_sync_outcome(const SyncOutcome& outcome);

} // namespace coherence

#endif // COHERENCE_DECISION_HPP
