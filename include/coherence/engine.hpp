// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// The coherence engine: the in-process owner of software-governed coherence
// authority, visibility and version progression.
//
// Concurrency contract
// --------------------
//  * A single non-recursive mutex guards all engine state. There are no read
//    locks to upgrade, no nested acquisition, and no re-entrancy.
//  * No user callback, virtual dispatch into user code, logging sink, or
//    blocking I/O is ever performed while the mutex is held. Durability I/O is
//    performed with the lock released, using a staged plan that is re-validated
//    under the lock before it is allowed to take effect. This is what makes the
//    mutate -> persist -> publish ordering real rather than aspirational.
//  * Every public operation is safe to call concurrently from any thread.
#ifndef COHERENCE_ENGINE_HPP
#define COHERENCE_ENGINE_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coherence/decision.hpp"
#include "coherence/enums.hpp"
#include "coherence/evidence.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/model.hpp"
#include "coherence/policy.hpp"
#include "coherence/status.hpp"

namespace coherence {

class DurableStore;

struct COHERENCE_API EngineConfig {
  CoordinatorEpoch initial_epoch = CoordinatorEpoch::from_value(1);

  std::uint64_t max_domains = 64;
  std::uint64_t max_participants = 65536;
  std::uint64_t max_objects = 1000000;
  std::uint64_t max_regions = 4000000;
  std::uint64_t max_regions_per_object = 1000000;
  std::uint64_t max_reads_per_object = 1000000;
  std::uint64_t max_object_length = 1ull << 40;   // 1 TiB logical extent
  std::uint64_t max_outstanding_invalidations = 1000000;
  std::uint64_t max_inflight_syncs = 1000000;

  /// Maximum number of recently completed publication requests retained per
  /// object for idempotent retry. Bounded so that memory cannot grow without
  /// limit under adversarial replay.
  std::uint64_t max_retained_requests_per_object = 64;

  /// Maximum number of decisions retained in the deterministic decision log.
  std::uint64_t max_decision_log = 4096;

  /// When false no decision-log entry is recorded, which removes one allocation
  /// per operation. The structured decision itself is unaffected, so callers
  /// still receive full explainability for the operation they performed; only
  /// the bounded retrospective log is disabled.
  bool enable_decision_log = true;

  /// When false, durable state is never written even if a store is attached.
  /// Publications are then reported as ephemeral rather than durable.
  bool enable_durability = true;
};

/// Generations and identity supplied by a caller for an authority-bearing
/// operation. Every field participates in staleness rejection.
struct COHERENCE_API AuthorityContext {
  CoordinatorEpoch epoch;
  ParticipantId participant;
  ParticipantBootId boot;
  ObjectId object;
  ObjectGeneration object_generation;
  PolicyGeneration policy_generation;
  RequestId request;
};

struct COHERENCE_API ReadRequest {
  AuthorityContext context;
  /// Optional explicit region to read from. When nil the engine selects a
  /// current replica deterministically.
  RegionId region;
  RegionGeneration region_generation;
  /// Explicit snapshot version for Snapshot consistency.
  VersionId snapshot_version;
  /// When true the caller demands current contents and will not accept a
  /// stale-allowed outcome.
  bool require_current = true;
};

struct COHERENCE_API WriteRequest {
  AuthorityContext context;
  /// Region the writer intends to mutate. Must belong to the caller.
  RegionId region;
  RegionGeneration region_generation;
};

struct COHERENCE_API DirtyRequest {
  AuthorityContext context;
  RegionId region;
  RegionGeneration region_generation;
  OwnershipGeneration ownership_generation;
  VersionId base_version;
  /// Fingerprint of the mutated bytes when the caller can compute one.
  ContentFingerprint content;
};

struct COHERENCE_API PublishRequest {
  AuthorityContext context;
  OwnershipGeneration ownership_generation;
  RegionId region;
  RegionGeneration region_generation;
  VersionId expected_base_version;
  ContentFingerprint content;
  /// When true the publication commits even if the writer holds no dirty
  /// region (used for the initial establishment of an authoritative version
  /// from an existing current replica).
  bool allow_without_dirty = false;
};

struct COHERENCE_API ReleaseRequest {
  AuthorityContext context;
  LeaseId lease;
  OwnershipGeneration ownership_generation;
  bool release_write_authority = false;
};

struct COHERENCE_API InvalidationRequest {
  AuthorityContext context;
  RegionId target_region;
  RegionGeneration target_region_generation;
  ReplicaGeneration target_replica_generation;
  OwnershipGeneration ownership_generation;
  VersionId published_version;
  bool acknowledgement_required = true;
};

struct COHERENCE_API InvalidationAck {
  AuthorityContext context;
  InvalidationId invalidation;
  RegionId region;
  RegionGeneration region_generation;
  ReplicaGeneration replica_generation;
  VersionId superseded_version;
};

struct COHERENCE_API SyncRequest {
  AuthorityContext context;
  SyncOperationKind kind = SyncOperationKind::Copy;
  RegionId source_region;
  RegionGeneration source_region_generation;
  RegionId destination_region;
  RegionGeneration destination_region_generation;
  OwnershipGeneration ownership_generation;
  std::string transport;
};

/// Re-establish an observation about a replica's current contents. A region
/// whose dynamic evidence was lost (restart, boot change, epoch advance) can
/// only become current again through a new observation; persisted metadata is
/// never sufficient.
struct COHERENCE_API RevalidateRequest {
  AuthorityContext context;
  RegionId region;
  RegionGeneration region_generation;
  VersionId observed_version;
  ContentFingerprint content;
  /// True when the caller actually compared real bytes rather than merely
  /// asserting a version. Assertions produce weaker evidence and are refused
  /// where the policy requires a content fingerprint.
  bool byte_compared = true;
};

struct COHERENCE_API SyncCompleteRequest {
  AuthorityContext context;
  SyncOperationId operation;
  RegionId destination_region;
  RegionGeneration destination_region_generation;
  VersionId destination_new_version;
  /// Fingerprint of the bytes actually observed at the destination. The engine
  /// refuses to certify currentness when this does not match the publication.
  ContentFingerprint observed_content;
  bool content_moved = true;
};

struct COHERENCE_API SyncFailRequest {
  AuthorityContext context;
  SyncOperationId operation;
  StatusCode cause = StatusCode::TransportFailure;
  bool outcome_unknown = false;
  std::string detail;
};

struct COHERENCE_API RegionRegistration {
  AuthorityContext context;
  CoherenceDomainId domain;
  RegionId requested_id;
  std::string name;
  MemoryDomain memory_domain = MemoryDomain::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Unsupported;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::uint64_t address_hint = 0;
  ReplicaId requested_replica;
  bool declared_writable = true;
  /// Version the region contents already correspond to. Nil means "no version
  /// known", which is recorded as Unknown and never treated as current.
  VersionId initial_version;
  ContentFingerprint content;
};

// ---------------------------------------------------------------------------
// Snapshot / inspection.
// ---------------------------------------------------------------------------
struct COHERENCE_API SnapshotOptions {
  bool include_regions = true;
  bool include_reads = true;
  bool include_invalidations = true;
  bool include_syncs = true;
  bool include_participants = true;
  ObjectId object_filter;  ///< nil means all objects
  std::uint64_t max_objects = 0;  ///< 0 means unlimited
};

struct COHERENCE_API CoherenceSnapshot {
  CoordinatorEpoch epoch;
  OperationSequence sequence;
  std::string build;
  std::vector<DomainRecord> domains;
  std::vector<ParticipantRecord> participants;
  std::vector<ObjectRecord> objects;
  std::vector<RegionRecord> regions;
  std::vector<InvalidationRecord> invalidations;
  std::vector<SyncRecord> syncs;
  bool shutting_down = false;

  [[nodiscard]] std::string render() const;
};

// ---------------------------------------------------------------------------
// Invariant audit.
// ---------------------------------------------------------------------------
struct COHERENCE_API AuditFinding {
  StatusCode code = StatusCode::Ok;
  std::string detail;
  ObjectId object;
  RegionId region;
  ParticipantId participant;
  InvalidationId invalidation;
  SyncOperationId sync;
};

struct COHERENCE_API AuditReport {
  bool clean = true;
  CoordinatorEpoch epoch;
  OperationSequence sequence;
  std::uint64_t objects_checked = 0;
  std::uint64_t regions_checked = 0;
  std::uint64_t participants_checked = 0;
  std::uint64_t invalidations_checked = 0;
  std::uint64_t syncs_checked = 0;
  std::vector<AuditFinding> findings;

  [[nodiscard]] std::string render() const;
};

// ---------------------------------------------------------------------------
// Recovery.
// ---------------------------------------------------------------------------
struct COHERENCE_API RecoveryReport {
  bool performed = false;
  CoordinatorEpoch previous_epoch;
  CoordinatorEpoch new_epoch;
  std::uint64_t objects_restored = 0;
  std::uint64_t regions_restored = 0;
  std::uint64_t participants_restored = 0;
  std::uint64_t regions_downgraded = 0;
  std::uint64_t authority_revoked = 0;
  std::uint64_t publications_completed = 0;
  std::uint64_t publications_requiring_operator = 0;
  std::uint64_t syncs_classified_unknown = 0;
  std::uint64_t invalidations_superseded = 0;
  std::uint64_t journal_records_replayed = 0;
  std::uint64_t corrupt_records_skipped = 0;
  std::vector<std::string> notes;

  [[nodiscard]] std::string render() const;
};

// ---------------------------------------------------------------------------
// The engine.
// ---------------------------------------------------------------------------
class COHERENCE_API CoherenceEngine {
 public:
  explicit CoherenceEngine(EngineConfig config = EngineConfig{});
  ~CoherenceEngine();

  CoherenceEngine(const CoherenceEngine&) = delete;
  CoherenceEngine& operator=(const CoherenceEngine&) = delete;

  // --- durability ---------------------------------------------------------
  /// Attach a durable store and perform recovery from it. The store must
  /// already be initialised. Recovery advances the coordinator epoch, fences
  /// every pre-restart boot, and downgrades all dynamic region currentness.
  Result<RecoveryReport> attach_store(std::shared_ptr<DurableStore> store);
  Status flush_durable();

  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] OperationSequence sequence() const;
  [[nodiscard]] bool shutting_down() const;
  [[nodiscard]] const EngineConfig& config() const noexcept;

  // --- domains ------------------------------------------------------------
  Result<DomainRecord> create_domain(std::string name);
  Result<DomainRecord> get_domain(CoherenceDomainId id) const;

  // --- participants -------------------------------------------------------
  Result<ParticipantRecord> register_participant(std::string name, ParticipantBootId boot,
                                                 CoordinatorEpoch epoch,
                                                 std::string node_label = std::string());
  Result<ParticipantRecord> get_participant(ParticipantId id) const;
  Result<ParticipantRecord> find_participant(std::string_view name) const;
  Result<ParticipantRecord> fence_participant(ParticipantId id, CoordinatorEpoch epoch,
                                              std::string reason);
  Result<ParticipantRecord> mark_session_closed(ParticipantId id, ParticipantBootId boot,
                                                CoordinatorEpoch epoch);

  // --- objects ------------------------------------------------------------
  Result<ObjectRecord> register_object(CoherenceDomainId domain, std::string name,
                                       std::uint64_t length, const CoherencePolicy& policy);
  Result<ObjectRecord> get_object(ObjectId id) const;
  Result<ObjectRecord> find_object(CoherenceDomainId domain, std::string_view name) const;
  Result<CoherencePolicy> get_policy(ObjectId id) const;
  Result<ObjectRecord> set_policy(ObjectId id, ObjectGeneration generation,
                                  PolicyGeneration expected_generation,
                                  const CoherencePolicy& next);

  // --- regions ------------------------------------------------------------
  Result<RegionRecord> register_region(const RegionRegistration& registration);
  Result<RegionRecord> get_region(RegionId id) const;
  Status retire_region(RegionId id, RegionGeneration generation, CoordinatorEpoch epoch);

  // --- coherence operations ----------------------------------------------
  Result<ReadDecision> acquire_read(const ReadRequest& request);
  Result<WriteGrant> acquire_write(const WriteRequest& request);
  Result<DirtyCondition> mark_dirty(const DirtyRequest& request);
  Result<PublicationReceipt> publish(const PublishRequest& request);
  Result<ReleaseOutcome> release(const ReleaseRequest& request);

  Result<InvalidationOutcome> invalidate(const InvalidationRequest& request);
  Result<InvalidationOutcome> acknowledge_invalidation(const InvalidationAck& acknowledgement);
  Result<SyncOutcome> begin_sync(const SyncRequest& request);
  Result<SyncOutcome> complete_sync(const SyncCompleteRequest& request);
  Result<SyncOutcome> fail_sync(const SyncFailRequest& request);
  Result<RegionRecord> revalidate_region(const RevalidateRequest& request);

  /// Resolve an object that recovery placed in the RecoveryRequired lifecycle.
  /// The newest state was never published and its only holder is gone, so the
  /// resolution records the loss explicitly and returns the object to service
  /// at its last authoritative version. It never invents a publication.
  Result<ObjectRecord> resolve_recovery(ObjectId id, ObjectGeneration generation,
                                        CoordinatorEpoch epoch, std::string note);

  // --- object retirement --------------------------------------------------
  Status retire_object(ObjectId id, ObjectGeneration generation, CoordinatorEpoch epoch);

  // --- inspection ---------------------------------------------------------
  Result<CoherenceSnapshot> snapshot(const SnapshotOptions& options = SnapshotOptions{}) const;
  Result<ReadDecision> explain_read(const ReadRequest& request) const;
  AuditReport audit() const;
  std::string render_decision_log() const;

  // --- shutdown -----------------------------------------------------------
  /// Revoke new authority and settle pending work deterministically. Idempotent.
  Status begin_shutdown(CoordinatorEpoch epoch);
  Status complete_shutdown(CoordinatorEpoch epoch);

 public:
  /// Opaque internal engine state. Declared publicly only so that the internal
  /// implementation translation units can name it; it is not part of the
  /// supported public API and changes without notice.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

} // namespace coherence

#endif // COHERENCE_ENGINE_HPP
