// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// The coherence model: logical objects, physical regions/replicas,
// participants and domains.
//
// A logical coherence object and its physical manifestations are separate
// entities. An address is never an object identity: the same address reused
// later never resurrects an earlier coherence object, because regions carry
// their own RegionGeneration and bind to an explicit ObjectGeneration.
#ifndef COHERENCE_MODEL_HPP
#define COHERENCE_MODEL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/enums.hpp"
#include "coherence/evidence.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/policy.hpp"

namespace coherence {

/// Fingerprint of real contents. Two fingerprints must match on both the
/// 128-bit digest and the CRC-32C before the runtime accepts that a
/// synchronization moved the bytes it claims to have moved.
struct COHERENCE_API ContentFingerprint {
  ContentDigest digest;
  std::uint32_t crc32c = 0;
  std::uint64_t length = 0;
  bool defined = false;

  friend bool operator==(const ContentFingerprint& a, const ContentFingerprint& b) {
    if (a.defined != b.defined) return false;
    if (!a.defined) return true;
    return a.length == b.length && a.crc32c == b.crc32c && a.digest == b.digest;
  }

  [[nodiscard]] std::string to_string() const;
};

/// Compute a fingerprint from real bytes.
[[nodiscard]] COHERENCE_API ContentFingerprint fingerprint_bytes(ByteSpan bytes);

struct COHERENCE_API DomainRecord {
  CoherenceDomainId id;
  std::string name;
  std::uint64_t generation = 1;
  DomainLifecycle lifecycle = DomainLifecycle::Created;
  CoordinatorEpoch created_epoch;
  CoordinatorEpoch current_epoch;
};

struct COHERENCE_API ParticipantRecord {
  ParticipantId id;
  /// Durable identity key. Re-admission under the same key with a fresh boot is
  /// a new incarnation; the previous boot is fenced and never revived.
  std::string name;
  ParticipantBootId boot;
  ParticipantBootId previous_boot;
  ParticipantLifecycle lifecycle = ParticipantLifecycle::Observed;
  CoordinatorEpoch admitted_epoch;
  CoordinatorEpoch last_epoch;
  SessionId session;
  OperationSequence last_sequence;
  bool live = false;
  std::uint64_t region_count = 0;
  std::string node_label;
  /// Set when a previous incarnation was fenced; explains stale-boot rejection.
  std::string fence_reason;
};

/// A read-authority grant. Revocable by LeaseId.
struct COHERENCE_API ReadGrant {
  LeaseId lease;
  ParticipantId participant;
  ParticipantBootId boot;
  ObjectId object;
  ObjectGeneration object_generation;
  OwnershipGeneration ownership_generation;
  VersionId version;
  RegionId region;
  RegionGeneration region_generation;
  OperationSequence granted_sequence;
  bool released = false;
};

struct COHERENCE_API RegionRecord {
  RegionId id;
  RegionGeneration generation;
  ReplicaId replica;
  ReplicaGeneration replica_generation;
  CoherenceDomainId domain;
  ObjectId object;
  ObjectGeneration object_generation;

  ParticipantId participant;
  ParticipantBootId boot;

  MemoryDomain memory_domain = MemoryDomain::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unsupported;
  std::string name;

  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  /// Opaque numeric address reported by the owning participant. Recorded for
  /// diagnostics only; it is never used as identity.
  std::uint64_t address_hint = 0;

  bool declared_writable = true;
  RegionLifecycle lifecycle = RegionLifecycle::Registered;

  CoherenceState state = CoherenceState::Unknown;

  /// Version of the contents the replica currently holds.
  VersionId version;
  VersionId last_synced_version;
  VersionId last_published_version;

  OwnershipGeneration ownership_generation;
  InvalidationId last_invalidation;

  /// Outstanding invalidation targeting this exact replica generation.
  InvalidationId pending_invalidation;
  RegionGeneration pending_invalidation_target;
  bool invalidation_pending = false;

  EvidenceId evidence;
  EvidenceGeneration evidence_generation;

  DirtyCondition dirty = DirtyCondition::Clean;
  VersionId dirty_base_version;
  bool dirty_published = false;

  ContentFingerprint content;

  OperationSequence updated_sequence;
  std::string note;
};

/// A publication request that already committed. Retained durably so that a
/// retried request never produces a second independent version, even across a
/// coordinator restart.
struct COHERENCE_API CompletedPublicationRequest {
  RequestId request;
  VersionId version;
  PublicationId publication;
  OperationSequence committed_sequence;
};

struct COHERENCE_API ObjectRecord {
  ObjectId id;
  ObjectGeneration generation;
  CoherenceDomainId domain;
  std::string name;
  std::uint64_t length = 0;

  PolicyId policy;
  PolicyGeneration policy_generation;

  ObjectLifecycle lifecycle = ObjectLifecycle::Created;
  AuthorityMode authority = AuthorityMode::None;

  ParticipantId writer;
  ParticipantBootId writer_boot;
  OwnershipGeneration ownership_generation;

  /// Latest authoritative version. Monotonic within this object.
  VersionId authoritative_version;
  /// Latest version whose publication committed durably.
  VersionId published_version;
  PublicationId publication;
  /// Durable commit sequence number for the authoritative version.
  OperationSequence committed_sequence;

  PublicationId pending_publication;
  VersionId pending_version;
  PublicationState publication_state = PublicationState::None;
  ContentFingerprint pending_content;

  std::vector<RegionId> replicas;       ///< sorted ascending
  /// Cached lowest-identity replica that was current at the last observation.
  /// Purely an accelerator: it is always re-validated against the live region
  /// state before it is used, so a stale cache can only cost a scan, never
  /// produce a wrong decision.
  RegionId preferred_current_replica;
  std::vector<ReadGrant> reads;         ///< sorted by lease id
  std::vector<InvalidationId> outstanding_invalidations;  ///< sorted
  /// Bounded ring of committed publication requests, ordered oldest first.
  std::vector<CompletedPublicationRequest> completed_requests;

  ParticipantId last_writer;
  ParticipantBootId last_writer_boot;
  bool has_unpublished_dirty = false;
  DirtyCondition dirty_condition = DirtyCondition::Clean;

  OperationSequence updated_sequence;
  std::string recovery_note;
};

struct COHERENCE_API InvalidationRecord {
  InvalidationId id;
  ObjectId object;
  ObjectGeneration object_generation;
  RegionId region;
  RegionGeneration region_generation;
  ReplicaGeneration replica_generation;
  VersionId published_version;
  VersionId superseded_version;
  OwnershipGeneration ownership_generation;
  CoordinatorEpoch epoch;
  ParticipantId target_participant;
  ParticipantBootId target_boot;
  InvalidationState state = InvalidationState::Requested;
  bool acknowledgement_required = true;
  OperationSequence issued_sequence;
  OperationSequence settled_sequence;
  std::string rationale;
};

struct COHERENCE_API SyncRecord {
  SyncOperationId id;
  SyncOperationKind kind = SyncOperationKind::Copy;
  ObjectId object;
  ObjectGeneration object_generation;
  RegionId source_region;
  RegionGeneration source_region_generation;
  RegionId destination_region;
  RegionGeneration destination_region_generation;
  VersionId source_version;
  VersionId destination_prior_version;
  /// Byte extent of the source replica that the transfer must move. A transfer
  /// that moves the wrong extent is detected by the content fingerprint check.
  std::uint64_t extent_offset = 0;
  std::uint64_t extent_length = 0;
  ContentFingerprint expected_content;
  OwnershipGeneration ownership_generation;
  PolicyId policy;
  PolicyGeneration policy_generation;
  CoordinatorEpoch epoch;
  ParticipantId destination_participant;
  ParticipantBootId destination_boot;
  SyncState state = SyncState::Planned;
  std::string postcondition;
  std::string transport;
  OperationSequence issued_sequence;
  OperationSequence settled_sequence;
  std::string failure_reason;
};

/// A publication that has been durably recorded but not yet made visible.
/// Recovery uses this record to complete the commit under the SAME
/// PublicationId and VersionId instead of inventing a second version.
struct COHERENCE_API PendingPublication {
  PublicationId id;
  ObjectId object;
  ObjectGeneration object_generation;
  VersionId version;
  VersionId prior_version;
  OwnershipGeneration ownership_generation;
  CoordinatorEpoch epoch;
  ParticipantId writer;
  ParticipantBootId writer_boot;
  RequestId request;
  ContentFingerprint content;
  OperationSequence sequence;
  bool durable = false;
};

// ---------------------------------------------------------------------------
// Deterministic renderings used by inspection, the CLI and tests.
// ---------------------------------------------------------------------------
COHERENCE_API std::string render_object(const ObjectRecord& record);
COHERENCE_API std::string render_region(const RegionRecord& record);
COHERENCE_API std::string render_participant(const ParticipantRecord& record);
COHERENCE_API std::string render_invalidation(const InvalidationRecord& record);
COHERENCE_API std::string render_sync(const SyncRecord& record);

} // namespace coherence

#endif // COHERENCE_MODEL_HPP
