// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Engine core: state helpers, registration, inspection and shutdown.
#include <algorithm>
#include <string>
#include <utility>

#include "coherence/engine.hpp"
#include "engine_impl.hpp"

namespace coherence {
namespace {

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& map) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(map.size());
  for (const auto& entry : map) keys.push_back(entry.first);
  return keys;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
CoherenceEngine::CoherenceEngine(EngineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
  if (impl_->epoch.is_nil()) impl_->epoch = CoordinatorEpoch::from_value(1);
  if (impl_->epoch.is_nil()) impl_->epoch = CoordinatorEpoch::from_value(1);
  impl_->durable.epoch = impl_->epoch;
  if (impl_->config.max_decision_log == 0) impl_->config.max_decision_log = 1;
}

CoherenceEngine::~CoherenceEngine() = default;

const EngineConfig& CoherenceEngine::config() const noexcept { return impl_->config; }

CoordinatorEpoch CoherenceEngine::epoch() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->epoch;
}

OperationSequence CoherenceEngine::sequence() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->sequence;
}

bool CoherenceEngine::shutting_down() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->shutting_down;
}

// ---------------------------------------------------------------------------
// Impl helpers
// ---------------------------------------------------------------------------
EvidenceRecord CoherenceEngine::Impl::make_evidence(
    EvidenceKind kind, EvidenceClass klass, ParticipantId participant, ParticipantBootId boot,
    ObjectId object, ObjectGeneration object_generation, RegionId region,
    RegionGeneration region_generation, VersionId version, const ContentFingerprint& content) {
  EvidenceRecord record;
  record.id = EvidenceId::from_value(next_evidence_id++);
  record.generation = EvidenceGeneration::from_value(1);
  record.kind = kind;
  record.evidence_class = klass;
  record.participant = participant;
  record.boot = boot;
  record.epoch = epoch;
  record.object = object;
  record.object_generation = object_generation;
  record.region = region;
  record.region_generation = region_generation;
  record.observed_version = version;
  if (content.defined) {
    record.observed_digest = content.digest;
    record.observed_crc32c = content.crc32c;
  }
  record.sequence = sequence;
  // Process-local observations only mean something inside the producing
  // process lifetime, so they are tagged and invalidated by a boot change.
  record.process_local = kind == EvidenceKind::ProcessLifecycle ||
                         kind == EvidenceKind::BackendCompletion ||
                         kind == EvidenceKind::RuntimeObservation;
  return record;
}

DecisionContext CoherenceEngine::Impl::make_context(const AuthorityContext& authority,
                                                    ObjectId object,
                                                    ObjectGeneration generation,
                                                    OwnershipGeneration ownership) {
  DecisionContext context;
  context.decision = DecisionId::from_value(next_decision_id++);
  context.epoch = epoch;
  context.object = object;
  context.object_generation = generation;
  context.ownership_generation = ownership;
  context.sequence = sequence;
  if (object.defined()) {
    const auto it = objects.find(object);
    if (it != objects.end()) {
      context.policy = it->second.policy;
      context.policy_generation = it->second.policy_generation;
    }
  } else {
    context.policy = PolicyId::nil();
  }
  context.policy_generation = authority.policy_generation.defined() ? authority.policy_generation
                                                                   : context.policy_generation;
  return context;
}

void CoherenceEngine::Impl::record_decision(DecisionKind kind, const DecisionContext& context,
                                            StatusCode code, std::string summary) {
  if (!config.enable_decision_log) return;
  DecisionRecord record;
  record.kind = kind;
  record.context = context;
  record.code = code;
  record.summary = std::move(summary);
  decision_log.push_back(std::move(record));
  ++decisions_recorded;
  while (decision_log.size() > config.max_decision_log) decision_log.pop_front();
}

std::string CoherenceEngine::Impl::render_decision_log_locked() const {
  std::string out;
  out.append("decision_log entries=");
  out.append(std::to_string(decision_log.size()));
  out.append(" total_recorded=");
  out.append(std::to_string(decisions_recorded));
  out.push_back('\n');
  for (const DecisionRecord& record : decision_log) {
    out.append("  decision=");
    out.append(record.context.decision.to_string());
    out.append(" kind=");
    out.append(to_token(record.kind));
    out.append(" code=");
    out.append(status_code_name(record.code));
    out.append(" epoch=");
    out.append(record.context.epoch.to_string());
    out.append(" object=");
    out.append(record.context.object.to_string());
    out.append(" ownership_generation=");
    out.append(record.context.ownership_generation.to_string());
    out.append(" policy_generation=");
    out.append(record.context.policy_generation.to_string());
    out.append(" summary=");
    out.append(record.summary);
    out.push_back('\n');
  }
  return out;
}

Status CoherenceEngine::Impl::append_locked(const JournalEntry& entry) {
  if (!config.enable_durability || store == nullptr) return Status::success();
  std::lock_guard<std::mutex> guard(store_mutex);
  JournalEntry stamped = entry;
  stamped.sequence = next_journal_sequence++;
  // The journal is written before the entry is allowed to influence in-memory
  // durable state, so durable state never claims more than the log contains.
  Status written = store->append(stamped);
  if (!written.ok()) {
    store_healthy = false;
    return written;
  }
  Status applied = apply_journal_entry(durable, stamped, store->limits());
  if (!applied.ok()) {
    store_healthy = false;
    return applied;
  }
  return Status::success();
}

Status CoherenceEngine::Impl::append_many_locked(const std::vector<JournalEntry>& entries) {
  if (!config.enable_durability || store == nullptr || entries.empty()) {
    return Status::success();
  }
  std::lock_guard<std::mutex> guard(store_mutex);
  std::vector<JournalEntry> stamped;
  stamped.reserve(entries.size());
  for (const JournalEntry& entry : entries) {
    JournalEntry copy = entry;
    copy.sequence = next_journal_sequence++;
    stamped.push_back(std::move(copy));
  }
  // Group commit: every entry is written before any of them is applied to
  // durable state, and a single barrier covers the whole group.
  Status written = store->append_batch(stamped);
  if (!written.ok()) {
    store_healthy = false;
    return written;
  }
  for (const JournalEntry& entry : stamped) {
    Status applied = apply_journal_entry(durable, entry, store->limits());
    if (!applied.ok()) {
      store_healthy = false;
      return applied;
    }
  }
  return Status::success();
}

void CoherenceEngine::Impl::upsert_durable_object_locked(const ObjectRecord& record) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertObject;
  entry.object = record;
  scrub_object_for_persistence(entry.object);
  (void)append_locked(entry);
}

void CoherenceEngine::Impl::upsert_durable_region_locked(const RegionRecord& record) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertRegion;
  entry.region = record;
  (void)append_locked(entry);
}

void CoherenceEngine::Impl::upsert_durable_participant_locked(const ParticipantRecord& record) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertParticipant;
  entry.participant = record;
  scrub_participant_for_persistence(entry.participant);
  (void)append_locked(entry);
}

Status CoherenceEngine::Impl::maybe_compact_locked() {
  if (!config.enable_durability || store == nullptr) return Status::success();
  // A compaction must never run while a publication reservation exists: the
  // reservation is deliberately excluded from durable state until its record
  // has actually been appended.
  if (!in_flight_publications.empty()) return Status::success();
  std::lock_guard<std::mutex> guard(store_mutex);
  if (store->journal_bytes() < compaction_threshold_bytes) return Status::success();
  Status compacted = store->compact(durable);
  if (!compacted.ok()) {
    store_healthy = false;
    return compacted;
  }
  return Status::success();
}

void CoherenceEngine::Impl::compact_locked() {
  if (!config.enable_durability || store == nullptr) return;
  if (!in_flight_publications.empty()) return;
  std::lock_guard<std::mutex> guard(store_mutex);
  (void)store->compact(durable);
}

std::vector<RegionId> CoherenceEngine::Impl::object_replica_ids_locked(ObjectId object) const {
  const auto it = objects.find(object);
  if (it == objects.end()) return {};
  return it->second.replicas;
}

bool CoherenceEngine::Impl::has_outstanding_invalidations_locked(ObjectId object) const {
  const auto it = objects.find(object);
  if (it == objects.end()) return false;
  for (const InvalidationId id : it->second.outstanding_invalidations) {
    const auto inv = invalidations.find(id);
    if (inv == invalidations.end()) continue;
    if (inv->second.state == InvalidationState::Requested) return true;
  }
  return false;
}

void CoherenceEngine::Impl::mark_regions_stale_locked(ObjectId object, VersionId authoritative,
                                                      RegionId except_region) {
  const auto object_it = objects.find(object);
  if (object_it == objects.end()) return;
  for (const RegionId region_id : object_it->second.replicas) {
    if (region_id == except_region) continue;
    const auto region_it = regions.find(region_id);
    if (region_it == regions.end()) continue;
    RegionRecord& region = region_it->second;
    if (region.state == CoherenceState::Current) {
      region.state = CoherenceState::Stale;
      region.note = "superseded by authoritative version " + authoritative.to_string();
      region.evidence = EvidenceId::nil();
      region.evidence_generation = EvidenceGeneration::nil();
      upsert_durable_region_locked(region);
    }
  }
}

void CoherenceEngine::Impl::release_reads_for_participant_locked(ParticipantId participant,
                                                                 ParticipantBootId boot) {
  for (auto& [object_id, object] : objects) {
    (void)object_id;
    bool changed = false;
    for (ReadGrant& grant : object.reads) {
      if (grant.released) continue;
      if (grant.participant != participant) continue;
      if (boot.defined() && grant.boot != boot) continue;
      grant.released = true;
      changed = true;
    }
    if (changed) {
      object.updated_sequence = sequence;
      upsert_durable_object_locked(object);
    }
  }
}

void CoherenceEngine::Impl::recompute_authority_locked(ObjectRecord& object) {
  const bool any_live_reads = std::any_of(
      object.reads.begin(), object.reads.end(),
      [](const ReadGrant& grant) { return !grant.released; });
  if (object.authority == AuthorityMode::ExclusiveWriter) return;
  // RevalidationRequired is an explicit statement that dynamic authority was
  // lost. Recomputing must not quietly downgrade it to a normal state.
  if (object.authority == AuthorityMode::RevalidationRequired) return;
  if (object.authority == AuthorityMode::TransferPending) {
    if (!has_outstanding_invalidations_locked(object.id)) {
      object.authority = AuthorityMode::ExclusiveWriter;
    }
    return;
  }
  object.authority = any_live_reads ? AuthorityMode::SharedReaders : AuthorityMode::None;
}

void CoherenceEngine::Impl::release_region_name_locked(const RegionRecord& record) {
  const auto it = region_index.find(RegionNameKey{record.object, record.name});
  if (it != region_index.end() && it->second == record.id) {
    region_index.erase(it);
  }
}

void CoherenceEngine::Impl::retotal_participant_regions_locked() {
  for (auto& [id, participant] : participants) {
    (void)id;
    participant.region_count = 0;
  }
  for (const auto& [id, region] : regions) {
    (void)id;
    const auto it = participants.find(region.participant);
    if (it != participants.end()) it->second.region_count += 1;
  }
}

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------
Result<DomainRecord> CoherenceEngine::create_domain(std::string name) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->shutting_down) {
    return Result<DomainRecord>::failure(
        Status(StatusCode::ShuttingDown, "the coherence engine is shutting down"));
  }
  if (name.empty()) {
    return Result<DomainRecord>::failure(
        Status(StatusCode::InvalidArgument, "domain name must not be empty"));
  }
  if (impl_->domain_index.count(name) != 0) {
    return Result<DomainRecord>::failure(
        Status(StatusCode::DuplicateIdentity, "a domain with this name already exists", name));
  }
  if (impl_->domains.size() >= impl_->config.max_domains) {
    return Result<DomainRecord>::failure(Status(StatusCode::CapacityExceeded,
                                                "domain capacity reached",
                                                std::to_string(impl_->config.max_domains)));
  }

  DomainRecord record;
  record.id = CoherenceDomainId::from_value(impl_->next_domain_id++);
  record.name = name;
  record.generation = 1;
  record.lifecycle = DomainLifecycle::Active;
  record.created_epoch = impl_->epoch;
  record.current_epoch = impl_->epoch;

  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertDomain;
  entry.domain = record;
  Status written = impl_->append_locked(entry);
  if (!written.ok()) return Result<DomainRecord>::failure(written);

  impl_->domains.emplace(record.id, record);
  impl_->domain_index.emplace(record.name, record.id);
  impl_->tick();
  return Result<DomainRecord>::success(record);
}

Result<DomainRecord> CoherenceEngine::get_domain(CoherenceDomainId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->domains.find(id);
  if (it == impl_->domains.end()) {
    return Result<DomainRecord>::failure(Status(StatusCode::UnknownDomain, "no such domain",
                                               id.to_string()));
  }
  return Result<DomainRecord>::success(it->second);
}

// ---------------------------------------------------------------------------
// Participants
// ---------------------------------------------------------------------------
Result<ParticipantRecord> CoherenceEngine::register_participant(std::string name,
                                                                ParticipantBootId boot,
                                                                CoordinatorEpoch epoch,
                                                                std::string node_label) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->shutting_down) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::ShuttingDown, "the coherence engine is shutting down"));
  }
  if (name.empty()) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::InvalidArgument, "participant name must not be empty"));
  }
  if (!boot.defined()) {
    return Result<ParticipantRecord>::failure(Status(
        StatusCode::InvalidArgument,
        "participant boot identity must be a live cryptographic identity, not a nil value"));
  }
  if (epoch != impl_->epoch) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::StaleEpoch, "participant admission carries a stale coordinator epoch",
               "request=" + epoch.to_string() + " current=" + impl_->epoch.to_string()));
  }

  const auto existing = impl_->participant_index.find(name);
  if (existing == impl_->participant_index.end() &&
      impl_->participants.size() >= impl_->config.max_participants) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::CapacityExceeded, "participant capacity reached",
               std::to_string(impl_->config.max_participants)));
  }

  ParticipantRecord record;
  if (existing != impl_->participant_index.end()) {
    record = impl_->participants.at(existing->second);
    if (record.boot == boot && record.lifecycle != ParticipantLifecycle::Retired) {
      // Same incarnation re-registering: idempotent, no fence, no new identity.
      record.last_epoch = epoch;
      record.live = true;
      if (record.lifecycle == ParticipantLifecycle::Observed ||
          record.lifecycle == ParticipantLifecycle::Admitted) {
        record.lifecycle = ParticipantLifecycle::Active;
      }
      impl_->participants[record.id] = record;
      impl_->upsert_durable_participant_locked(record);
      impl_->tick();
      return Result<ParticipantRecord>::success(record);
    }
    if (record.lifecycle == ParticipantLifecycle::Retired) {
      return Result<ParticipantRecord>::failure(
          Status(StatusCode::Retired, "the participant identity has been retired", name));
    }
    // A new boot under the same durable name creates a distinct incarnation.
    // The previous incarnation is fenced permanently: authority held before the
    // restart is never restored. Its physical mappings died with the process,
    // so its replicas are retired rather than left addressable by a later
    // incarnation that no longer owns those bytes.
    const ParticipantBootId superseded_boot = record.boot;
    for (auto& [region_id, region] : impl_->regions) {
      (void)region_id;
      if (region.participant != record.id || region.boot != superseded_boot) continue;
      if (region.lifecycle == RegionLifecycle::Retired) continue;
      region.lifecycle = RegionLifecycle::Retired;
      region.state = CoherenceState::Retired;
      region.dirty = region.dirty == DirtyCondition::DirtyUnpublished ? DirtyCondition::DirtyLost
                                                                      : region.dirty;
      region.evidence = EvidenceId::nil();
      region.evidence_generation = EvidenceGeneration::nil();
      region.invalidation_pending = false;
      region.pending_invalidation = InvalidationId::nil();
      region.pending_invalidation_target = RegionGeneration::nil();
      region.updated_sequence = impl_->sequence;
      region.note = "retired when the owning participant was re-admitted under a new boot identity";
      impl_->release_region_name_locked(region);
      impl_->upsert_durable_region_locked(region);
    }
    record.previous_boot = record.boot;
    record.boot = boot;
    record.lifecycle = ParticipantLifecycle::Active;
    record.live = true;
    record.admitted_epoch = epoch;
    record.last_epoch = epoch;
    record.session = SessionId::nil();
    record.last_sequence = OperationSequence::nil();
    record.fence_reason = "superseded by a new participant boot identity";
  } else {
    record.id = ParticipantId::from_value(impl_->next_participant_id++);
    record.name = name;
    record.boot = boot;
    record.lifecycle = ParticipantLifecycle::Active;
    record.admitted_epoch = epoch;
    record.last_epoch = epoch;
    record.live = true;
  }
  record.node_label = std::move(node_label);

  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertParticipant;
  entry.participant = record;
  scrub_participant_for_persistence(entry.participant);
  Status written = impl_->append_locked(entry);
  if (!written.ok()) return Result<ParticipantRecord>::failure(written);

  impl_->participants[record.id] = record;
  impl_->participant_index[record.name] = record.id;
  impl_->tick();
  return Result<ParticipantRecord>::success(record);
}

Result<ParticipantRecord> CoherenceEngine::get_participant(ParticipantId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->participants.find(id);
  if (it == impl_->participants.end()) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::UnknownParticipant, "no such participant", id.to_string()));
  }
  return Result<ParticipantRecord>::success(it->second);
}

Result<ParticipantRecord> CoherenceEngine::find_participant(std::string_view name) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->participant_index.find(std::string(name));
  if (it == impl_->participant_index.end()) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::UnknownParticipant, "no such participant", std::string(name)));
  }
  return Result<ParticipantRecord>::success(impl_->participants.at(it->second));
}

Result<ParticipantRecord> CoherenceEngine::mark_session_closed(ParticipantId id,
                                                               ParticipantBootId boot,
                                                               CoordinatorEpoch) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->participants.find(id);
  if (it == impl_->participants.end()) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::UnknownParticipant, "no such participant", id.to_string()));
  }
  ParticipantRecord& record = it->second;
  if (record.boot != boot) {
    return Result<ParticipantRecord>::failure(Status(
        StatusCode::StaleBoot, "session close carries a boot identity that is not the live one",
        "request=" + boot.to_string() + " live=" + record.boot.to_string()));
  }
  record.live = false;
  if (record.lifecycle == ParticipantLifecycle::Active) {
    record.lifecycle = ParticipantLifecycle::Degraded;
  }
  impl_->upsert_durable_participant_locked(record);
  impl_->tick();
  const ParticipantRecord copy = record;
  return Result<ParticipantRecord>::success(copy);
}

Result<ParticipantRecord> CoherenceEngine::fence_participant(ParticipantId id,
                                                             CoordinatorEpoch epoch,
                                                             std::string reason) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->participants.find(id);
  if (it == impl_->participants.end()) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::UnknownParticipant, "no such participant", id.to_string()));
  }
  if (epoch != impl_->epoch) {
    return Result<ParticipantRecord>::failure(Status(
        StatusCode::StaleEpoch, "fence request carries a stale coordinator epoch",
        "request=" + epoch.to_string() + " current=" + impl_->epoch.to_string()));
  }
  ParticipantRecord& participant = it->second;
  if (participant.lifecycle == ParticipantLifecycle::Retired) {
    return Result<ParticipantRecord>::failure(
        Status(StatusCode::Retired, "the participant is already retired", participant.name));
  }
  const ParticipantBootId boot = participant.boot;

  // Note on ordering: a fence always takes effect in memory even if the durable
  // record cannot be written. Failing to revoke authority because a disk write
  // failed would be strictly less safe than failing to record the revocation,
  // so the error is reported and the store is marked unhealthy instead.
  impl_->tick();
  std::vector<JournalEntry> entries;

  // 1. Revoke write authority held by this incarnation.
  for (auto& [object_id, object] : impl_->objects) {
    (void)object_id;
    bool changed = false;
    if (object.writer == id && object.writer_boot == boot) {
      object.authority = AuthorityMode::None;
      object.writer = ParticipantId::nil();
      object.writer_boot = ParticipantBootId::nil();
      if (object.publication_state == PublicationState::PendingDurable ||
          object.publication_state == PublicationState::RecoveryRequired) {
        object.publication_state = PublicationState::Aborted;
        object.pending_publication = PublicationId::nil();
        object.pending_version = VersionId::nil();
        object.pending_content = ContentFingerprint{};
        JournalEntry clear;
        clear.kind = JournalEntryKind::ClearPendingPublication;
        clear.remove_object = object.id;
        entries.push_back(clear);
      }
      // The only dirty authority holder disappeared before publication. The
      // runtime never invents a successful publication from this.
      if (object.has_unpublished_dirty) {
        object.has_unpublished_dirty = false;
        switch (impl_->policies.count(object.policy) != 0
                    ? impl_->policies.at(object.policy).dirty_loss_policy
                    : DirtyLossPolicy::ReportUnknown) {
          case DirtyLossPolicy::ReportLost:
            object.dirty_condition = DirtyCondition::DirtyLost;
            break;
          case DirtyLossPolicy::RequireRecovery:
            object.dirty_condition = DirtyCondition::DirtyUnknown;
            object.lifecycle = ObjectLifecycle::RecoveryRequired;
            object.recovery_note =
                "dirty authority holder was fenced before publication; recoverable state is "
                "unknown";
            break;
          case DirtyLossPolicy::ReportUnknown:
          case DirtyLossPolicy::Unspecified:
          default:
            object.dirty_condition = DirtyCondition::DirtyUnknown;
            break;
        }
      }
      object.updated_sequence = impl_->sequence;
      changed = true;
    }
    if (changed) {
      ObjectRecord durable_copy = object;
      scrub_object_for_persistence(durable_copy);
      JournalEntry entry;
      entry.kind = JournalEntryKind::UpsertObject;
      entry.object = std::move(durable_copy);
      entries.push_back(std::move(entry));
    }
  }

  // 2. Fence the participant's regions. Fenced contents are never a source of
  //    truth again for that incarnation.
  for (auto& [region_id, region] : impl_->regions) {
    (void)region_id;
    if (region.participant != id || region.boot != boot) continue;
    if (region.state == CoherenceState::Retired) continue;
    region.state = CoherenceState::Fenced;
    if (region.dirty == DirtyCondition::DirtyUnpublished) {
      region.dirty = DirtyCondition::DirtyLost;
    }
    region.evidence = EvidenceId::nil();
    region.evidence_generation = EvidenceGeneration::nil();
    region.note = "owning participant was fenced: " + reason;
    region.updated_sequence = impl_->sequence;
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertRegion;
    entry.region = region;
    entries.push_back(std::move(entry));
  }

  // 3. In-flight synchronizations to this participant have an indeterminate
  //    outcome and are recorded as such rather than assumed to have failed.
  for (auto& [sync_id, sync] : impl_->syncs) {
    (void)sync_id;
    if (sync.destination_participant != id || sync.destination_boot != boot) continue;
    if (sync.state == SyncState::Completed || sync.state == SyncState::Cancelled ||
        sync.state == SyncState::Failed) {
      continue;
    }
    sync.state = SyncState::OutcomeUnknown;
    sync.settled_sequence = impl_->sequence;
    sync.failure_reason = "destination participant was fenced before synchronization completed";
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertSync;
    entry.sync = sync;
    entries.push_back(std::move(entry));
  }

  // 4. Outstanding invalidations that targeted this incarnation's replicas are
  //    satisfied vacuously and are never applied to a later generation.
  for (auto& [invalidation_id, invalidation] : impl_->invalidations) {
    (void)invalidation_id;
    if (invalidation.target_participant != id || invalidation.target_boot != boot) continue;
    if (invalidation.state != InvalidationState::Requested) continue;
    invalidation.state = InvalidationState::Superseded;
    invalidation.settled_sequence = impl_->sequence;
    invalidation.rationale = "target replica generation disappeared with the fenced participant";
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertInvalidation;
    entry.invalidation = invalidation;
    entries.push_back(std::move(entry));
  }

  // 5. Release the participant's read grants.
  impl_->release_reads_for_participant_locked(id, boot);

  for (auto& [object_id, object] : impl_->objects) {
    (void)object_id;
    impl_->recompute_authority_locked(object);
  }

  participant.lifecycle = ParticipantLifecycle::Fenced;
  participant.live = false;
  participant.fence_reason = reason;
  participant.last_epoch = epoch;
  JournalEntry participant_entry;
  participant_entry.kind = JournalEntryKind::UpsertParticipant;
  participant_entry.participant = participant;
  scrub_participant_for_persistence(participant_entry.participant);
  entries.push_back(std::move(participant_entry));

  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) return Result<ParticipantRecord>::failure(written);

  impl_->tick();
  const ParticipantRecord copy = participant;
  return Result<ParticipantRecord>::success(copy);
}

} // namespace coherence
