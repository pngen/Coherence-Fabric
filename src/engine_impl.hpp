// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Internal engine state. Not installed.
#ifndef COHERENCE_SRC_ENGINE_IMPL_HPP
#define COHERENCE_SRC_ENGINE_IMPL_HPP

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"

namespace coherence {

/// Names used to build deterministic lookup indexes. Domain-scoped so that two
/// domains may each contain an object called "weights" without ambiguity.
struct ObjectNameKey {
  CoherenceDomainId domain;
  std::string name;

  friend bool operator<(const ObjectNameKey& a, const ObjectNameKey& b) {
    if (a.domain != b.domain) return a.domain < b.domain;
    return a.name < b.name;
  }
};

struct RegionNameKey {
  ObjectId object;
  std::string name;

  friend bool operator<(const RegionNameKey& a, const RegionNameKey& b) {
    if (a.object != b.object) return a.object < b.object;
    return a.name < b.name;
  }
};

/// A single decision-log entry. Kept small and deterministic; the log is a
/// bounded ring so that an adversarial caller cannot grow memory without limit.
struct DecisionRecord {
  DecisionKind kind = DecisionKind::Audit;
  DecisionContext context;
  StatusCode code = StatusCode::Ok;
  std::string summary;
};

struct CoherenceEngine::Impl {
  explicit Impl(EngineConfig cfg) : config(std::move(cfg)) {}

  EngineConfig config;

  /// Guards every field below. Non-recursive by design: no engine operation
  /// calls back into the engine.
  mutable std::mutex mutex;

  CoordinatorEpoch epoch;
  OperationSequence sequence{OperationSequence::from_value(1)};
  bool shutting_down = false;
  bool shutdown_complete = false;

  // --- durable image -------------------------------------------------------
  DurableImage durable;

  // --- live-only state -----------------------------------------------------
  std::map<CoherenceDomainId, DomainRecord> domains;
  std::map<ParticipantId, ParticipantRecord> participants;
  std::map<ObjectId, ObjectRecord> objects;
  std::map<RegionId, RegionRecord> regions;
  std::map<InvalidationId, InvalidationRecord> invalidations;
  std::map<SyncOperationId, SyncRecord> syncs;
  std::map<PolicyId, CoherencePolicy> policies;

  std::map<std::string, CoherenceDomainId> domain_index;
  std::map<std::string, ParticipantId> participant_index;
  std::map<ObjectNameKey, ObjectId> object_index;
  std::map<RegionNameKey, RegionId> region_index;

  /// Publications whose durable record is being written. Excluded from
  /// compaction so that a compaction can never make an uncommitted publication
  /// durable, and can never drop a durable one.
  std::map<PublicationId, PendingPublication> in_flight_publications;

  // --- identity allocation -------------------------------------------------
  std::uint64_t next_domain_id = 1;
  std::uint64_t next_participant_id = 1;
  std::uint64_t next_object_id = 1;
  std::uint64_t next_region_id = 1;
  std::uint64_t next_replica_id = 1;
  std::uint64_t next_publication_id = 1;
  std::uint64_t next_invalidation_id = 1;
  std::uint64_t next_sync_id = 1;
  std::uint64_t next_lease_id = 1;
  std::uint64_t next_policy_id = 1;
  std::uint64_t next_evidence_id = 1;
  std::uint64_t next_decision_id = 1;

  // --- persistence ---------------------------------------------------------
  std::shared_ptr<DurableStore> store;
  /// Serialises store operations. Lock order is always mutex -> store_mutex.
  mutable std::mutex store_mutex;
  /// Monotonic journal log position. Reserved under `mutex` so that a
  /// publication reservation made without the lock still occupies a fixed slot.
  std::uint64_t next_journal_sequence = 1;
  std::uint64_t compaction_threshold_bytes = 8ull * 1024 * 1024;
  std::uint64_t journal_bytes_at_last_compaction = 0;
  bool store_healthy = true;
  bool recovery_performed = false;
  RecoveryReport recovery_report;

  // --- diagnostics ---------------------------------------------------------
  std::deque<DecisionRecord> decision_log;
  std::uint64_t decisions_recorded = 0;

  // --- helpers (see engine_core.cpp / engine_ops.cpp) ----------------------
  CoordinatorEpoch next_epoch() noexcept {
    epoch = epoch.next();
    return epoch;
  }
  OperationSequence tick() noexcept { return sequence = sequence.next(); }

  [[nodiscard]] EvidenceRecord make_evidence(EvidenceKind kind, EvidenceClass klass,
                                             ParticipantId participant, ParticipantBootId boot,
                                             ObjectId object, ObjectGeneration object_generation,
                                             RegionId region, RegionGeneration region_generation,
                                             VersionId version, const ContentFingerprint& content);
  [[nodiscard]] DecisionContext make_context(const AuthorityContext& authority, ObjectId object,
                                             ObjectGeneration generation,
                                             OwnershipGeneration ownership);
  void record_decision(DecisionKind kind, const DecisionContext& context, StatusCode code,
                       std::string summary);
  [[nodiscard]] std::string render_decision_log_locked() const;

  /// True when durable writes will actually be performed. Entry construction is
  /// skipped otherwise, which matters because an object record carries its full
  /// replica list.
  [[nodiscard]] bool durability_active() const noexcept {
    return config.enable_durability && store != nullptr;
  }

  Status append_locked(const JournalEntry& entry);
  Status append_many_locked(const std::vector<JournalEntry>& entries);
  Status maybe_compact_locked();
  void compact_locked();
  [[nodiscard]] DurableImage snapshot_image_locked() const;
  void upsert_durable_region_locked(const RegionRecord& record);
  void upsert_durable_object_locked(const ObjectRecord& record);
  void upsert_durable_participant_locked(const ParticipantRecord& record);

  void retotal_participant_regions_locked();
  /// Release the name a replica occupied so that a replacement incarnation can
  /// register under the same name. The retired record itself is preserved.
  void release_region_name_locked(const RegionRecord& record);
  [[nodiscard]] std::vector<RegionId> object_replica_ids_locked(ObjectId object) const;
  void mark_regions_stale_locked(ObjectId object, VersionId authoritative,
                                 RegionId except_region);
  void release_reads_for_participant_locked(ParticipantId participant, ParticipantBootId boot);
  void recompute_authority_locked(ObjectRecord& object);
  [[nodiscard]] bool has_outstanding_invalidations_locked(ObjectId object) const;
};

} // namespace coherence

#endif // COHERENCE_SRC_ENGINE_IMPL_HPP
