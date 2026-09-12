// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Durable recovery and the invariant auditor.
#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/version.hpp"
#include "engine_helpers.hpp"
#include "engine_impl.hpp"

namespace coherence {
namespace {

template <typename IdType>
void raise_counter(std::uint64_t& counter, IdType id) {
  if (id.defined() && id.value() + 1 > counter) counter = id.value() + 1;
}

AuditFinding finding(StatusCode code, std::string detail) {
  AuditFinding out;
  out.code = code;
  out.detail = std::move(detail);
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------
Result<RecoveryReport> CoherenceEngine::attach_store(std::shared_ptr<DurableStore> store) {
  if (store == nullptr) {
    return Result<RecoveryReport>::failure(
        Status(StatusCode::InvalidArgument, "durable store must not be null"));
  }
  RecoveryReport report;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->store != nullptr) {
    return Result<RecoveryReport>::failure(
        Status(StatusCode::InvalidState, "a durable store is already attached"));
  }
  // Attaching durable state replaces the in-memory domain, so it is only
  // meaningful on an engine that has not created anything yet. Refusing is far
  // safer than silently discarding registrations the caller already made.
  if (!impl_->domains.empty() || !impl_->objects.empty() || !impl_->regions.empty() ||
      !impl_->participants.empty()) {
    return Result<RecoveryReport>::failure(Status(
        StatusCode::InvalidState,
        "a durable store cannot be attached after coherence state has been created",
        "domains=" + std::to_string(impl_->domains.size()) +
            " objects=" + std::to_string(impl_->objects.size()) +
            " regions=" + std::to_string(impl_->regions.size())));
  }
  impl_->store = std::move(store);

  DurableImage image;
  Result<LoadReport> loaded = impl_->store->open(image);
  if (!loaded.has_value()) {
    impl_->store.reset();
    return Result<RecoveryReport>::failure(loaded.status());
  }
  const LoadReport& load = loaded.value();
  report.performed = true;
  report.journal_records_replayed = load.journal_records_replayed;
  report.corrupt_records_skipped = load.corrupt_records;
  report.previous_epoch = image.epoch;
  if (load.truncated_tail) {
    report.notes.push_back(
        "the journal ended in an incomplete or corrupt record; replay stopped at that point and "
        "everything after it was ignored");
  }

  // The coordinator epoch always advances across a restart. Every session,
  // authority grant and observation from before the restart is therefore
  // unusable, which is what fences stale actors.
  const std::uint64_t previous = image.epoch.defined() ? image.epoch.value() : 0;
  const std::uint64_t initial = impl_->config.initial_epoch.defined()
                                    ? impl_->config.initial_epoch.value()
                                    : 1;
  const std::uint64_t next = std::max(previous + 1, initial);
  impl_->epoch = CoordinatorEpoch::from_value(next);
  report.new_epoch = impl_->epoch;
  impl_->durable = image;
  impl_->durable.epoch = impl_->epoch;

  impl_->domains.clear();
  impl_->participants.clear();
  impl_->objects.clear();
  impl_->regions.clear();
  impl_->invalidations.clear();
  impl_->syncs.clear();
  impl_->policies.clear();
  impl_->domain_index.clear();
  impl_->participant_index.clear();
  impl_->object_index.clear();
  impl_->region_index.clear();

  // A durable record whose map key disagrees with the identity embedded in its
  // body is corrupt. It is ignored rather than trusted, and the discrepancy is
  // reported so that it cannot pass unnoticed.
  std::uint64_t identity_mismatches = 0;
  auto check_identity = [&identity_mismatches](auto key, auto embedded) {
    if (key != embedded) {
      ++identity_mismatches;
      return false;
    }
    return true;
  };

  for (const auto& [id, record] : image.domains) {
    if (id != record.id) {
      ++identity_mismatches;
      continue;
    }
    DomainRecord restored = record;
    restored.current_epoch = impl_->epoch;
    if (restored.lifecycle == DomainLifecycle::Quiescing) {
      restored.lifecycle = DomainLifecycle::RecoveryRequired;
    }
    impl_->domains.emplace(restored.id, restored);
    impl_->domain_index.emplace(restored.name, restored.id);
    raise_counter(impl_->next_domain_id, restored.id);
  }

  for (const auto& [id, record] : image.participants) {
    if (id != record.id) {
      ++identity_mismatches;
      continue;
    }
    ParticipantRecord restored = record;
    // Sessions never survive a restart. The durable identity remains, but the
    // incarnation is no longer live and must re-admit with its boot identity.
    restored.live = false;
    restored.session = SessionId::nil();
    restored.last_sequence = OperationSequence::nil();
    if (restored.lifecycle == ParticipantLifecycle::Active) {
      restored.lifecycle = ParticipantLifecycle::Degraded;
    }
    impl_->participants.emplace(restored.id, restored);
    impl_->participant_index.emplace(restored.name, restored.id);
    raise_counter(impl_->next_participant_id, restored.id);
    ++report.participants_restored;
  }

  for (const auto& [id, record] : image.policies) {
    if (!check_identity(id, record.id)) continue;
    impl_->policies.emplace(record.id, record);
    raise_counter(impl_->next_policy_id, record.id);
  }
  if (identity_mismatches != 0) {
    report.notes.push_back(std::to_string(identity_mismatches) +
                           " durable record(s) were ignored because their record key disagrees "
                           "with the identity embedded in the record body");
  }

  for (const auto& [id, record] : image.invalidations) {
    if (id != record.id) {
      ++identity_mismatches;
      continue;
    }
    InvalidationRecord restored = record;
    if (restored.state == InvalidationState::Requested) {
      restored.state = InvalidationState::Unknown;
      restored.settled_sequence = impl_->sequence;
      restored.rationale =
          "the coordinator restarted before the acknowledgement arrived; every replica was "
          "conservatively downgraded, which discharges the invalidation without claiming the "
          "acknowledgement was observed";
      ++report.invalidations_superseded;
    }
    impl_->invalidations.emplace(restored.id, restored);
    raise_counter(impl_->next_invalidation_id, restored.id);
  }

  for (const auto& [id, record] : image.syncs) {
    if (id != record.id) {
      ++identity_mismatches;
      continue;
    }
    SyncRecord restored = record;
    if (restored.state == SyncState::Requested || restored.state == SyncState::InProgress ||
        restored.state == SyncState::Planned || restored.state == SyncState::OutcomeUnknown) {
      restored.state = SyncState::OutcomeUnknown;
      restored.settled_sequence = impl_->sequence;
      restored.failure_reason =
          "the coordinator restarted while the transfer was in flight; whether bytes moved is "
          "unknown and is reported as unknown";
      ++report.syncs_classified_unknown;
    }
    impl_->syncs.emplace(restored.id, restored);
    raise_counter(impl_->next_sync_id, restored.id);
  }

  // Objects: dynamic authority is gone. Rebuild durable state and classify any
  // publication that was in flight.
  for (const auto& [id, stored] : image.objects) {
    if (id != stored.id) {
      ++identity_mismatches;
      continue;
    }
    ObjectRecord object = stored;
    raise_counter(impl_->next_object_id, object.id);
    object.authority = AuthorityMode::RevalidationRequired;
    object.writer = ParticipantId::nil();
    object.writer_boot = ParticipantBootId::nil();
    object.reads.clear();
    object.outstanding_invalidations.clear();
    object.updated_sequence = impl_->sequence;
    object.recovery_note =
        "dynamic authority was not restored: the coordinator epoch advanced from " +
        std::to_string(previous) + " to " + impl_->epoch.to_string();
    ++report.authority_revoked;

    const auto pending_it = image.pending_publications.find(object.pending_publication);
    const bool has_durable_pending =
        object.pending_publication.defined() && pending_it != image.pending_publications.end();
    const CoherencePolicy* policy =
        impl_->policies.count(object.policy) != 0 ? &impl_->policies.at(object.policy) : nullptr;

    if (has_durable_pending) {
      const PendingPublication& pending = pending_it->second;
      const RecoveryPolicy recovery =
          policy != nullptr ? policy->recovery_policy : RecoveryPolicy::Conservative;
      if (recovery == RecoveryPolicy::CompleteDurablePending) {
        // The durable record fixes the exact PublicationId and VersionId, so
        // recovery converges on that publication instead of inventing a second
        // independent version.
        object.authoritative_version = pending.version;
        object.published_version = pending.version;
        object.publication = pending.id;
        object.publication_state = PublicationState::Committed;
        object.committed_sequence = impl_->sequence;
        object.pending_publication = PublicationId::nil();
        object.pending_version = VersionId::nil();
        object.pending_content = ContentFingerprint{};
        object.has_unpublished_dirty = false;
        object.dirty_condition = DirtyCondition::DirtyPublished;
        object.last_writer = pending.writer;
        object.last_writer_boot = pending.writer_boot;
        if (pending.request.defined()) {
          object.completed_requests.push_back(CompletedPublicationRequest{
              pending.request, pending.version, pending.id, impl_->sequence});
        }
        object.recovery_note +=
            "; the durable pending publication was completed under its original publication and "
            "version identity";
        ++report.publications_completed;
      } else {
        object.publication_state = PublicationState::RecoveryRequired;
        object.lifecycle = ObjectLifecycle::RecoveryRequired;
        object.recovery_note +=
            "; a durable pending publication exists whose completion the active recovery policy "
            "does not authorise; an operator must resolve it";
        ++report.publications_requiring_operator;
      }
    } else if (object.publication_state == PublicationState::PendingDurable) {
      object.publication_state = PublicationState::Aborted;
      object.pending_publication = PublicationId::nil();
      object.pending_version = VersionId::nil();
      object.pending_content = ContentFingerprint{};
      object.recovery_note +=
          "; an in-flight publication had no durable record and was aborted rather than assumed "
          "to have committed";
    }

    if (object.has_unpublished_dirty) {
      object.has_unpublished_dirty = false;
      const DirtyLossPolicy dirty_policy =
          policy != nullptr ? policy->dirty_loss_policy : DirtyLossPolicy::ReportUnknown;
      switch (dirty_policy) {
        case DirtyLossPolicy::ReportLost:
          object.dirty_condition = DirtyCondition::DirtyLost;
          break;
        case DirtyLossPolicy::RequireRecovery:
          object.dirty_condition = DirtyCondition::DirtyUnknown;
          object.lifecycle = ObjectLifecycle::RecoveryRequired;
          break;
        case DirtyLossPolicy::ReportUnknown:
        case DirtyLossPolicy::Unspecified:
        default:
          object.dirty_condition = DirtyCondition::DirtyUnknown;
          break;
      }
      object.recovery_note +=
          "; an unpublished modification existed at restart and the runtime does not invent a "
          "successful publication for it";
    }

    if (object.lifecycle == ObjectLifecycle::Quiescing) {
      object.lifecycle = ObjectLifecycle::RecoveryRequired;
    }
    impl_->objects.emplace(object.id, object);
    impl_->object_index.emplace(ObjectNameKey{object.domain, object.name}, object.id);
    ++report.objects_restored;
  }

  // Regions: dynamic currentness never survives a restart. Persisted metadata
  // is provenance, not proof, so every replica is downgraded and receives
  // PersistedMetadata evidence that the evidence model refuses to treat as
  // proof of currentness.
  for (const auto& [id, stored] : image.regions) {
    if (id != stored.id) {
      ++identity_mismatches;
      continue;
    }
    RegionRecord region = stored;
    raise_counter(impl_->next_region_id, region.id);
    raise_counter(impl_->next_replica_id, region.replica);
    const bool terminal = region.state == CoherenceState::Retired ||
                          region.lifecycle == RegionLifecycle::Retired;
    const bool fenced = region.state == CoherenceState::Fenced;
    if (terminal) {
      region.state = CoherenceState::Retired;
    } else if (fenced) {
      region.state = CoherenceState::Fenced;
    } else {
      if (region.state != CoherenceState::RevalidationRequired) {
        ++report.regions_downgraded;
      }
      region.state = CoherenceState::RevalidationRequired;
    }
    region.invalidation_pending = false;
    region.pending_invalidation = InvalidationId::nil();
    region.pending_invalidation_target = RegionGeneration::nil();
    region.updated_sequence = impl_->sequence;
    if (!terminal) {
      const EvidenceRecord evidence = impl_->make_evidence(
          EvidenceKind::PersistedMetadata, region.evidence_class, region.participant, region.boot,
          region.object, region.object_generation, region.id, region.generation, region.version,
          region.content);
      region.evidence = evidence.id;
      region.evidence_generation = evidence.generation;
      region.note = "restored from durable metadata; currentness must be re-established by a new "
                    "observation";
    }
    impl_->regions.emplace(region.id, region);
    // A retired replica keeps its record but not its name, so a replacement
    // incarnation can register under the same name.
    if (region.lifecycle != RegionLifecycle::Retired) {
      impl_->region_index.emplace(RegionNameKey{region.object, region.name}, region.id);
    }
    ++report.regions_restored;
  }

  raise_counter(impl_->next_journal_sequence, OperationSequence::from_value(image.sequence));
  impl_->retotal_participant_regions_locked();

  // Persist the recovery outcome so the durable image matches the live state.
  std::vector<JournalEntry> entries;
  {
    JournalEntry entry;
    entry.kind = JournalEntryKind::SetEpoch;
    entry.epoch = impl_->epoch;
    entries.push_back(std::move(entry));
  }
  for (const auto& [id, record] : impl_->participants) {
    (void)id;
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertParticipant;
    entry.participant = record;
    scrub_participant_for_persistence(entry.participant);
    entries.push_back(std::move(entry));
  }
  for (const auto& [id, record] : impl_->objects) {
    (void)id;
    engine_detail::push_object_entry(entries, record);
  }
  for (const auto& [id, record] : impl_->regions) {
    (void)id;
    engine_detail::push_region_entry(entries, record);
  }
  for (const auto& [id, record] : impl_->invalidations) {
    (void)id;
    engine_detail::push_invalidation_entry(entries, record);
  }
  for (const auto& [id, record] : impl_->syncs) {
    (void)id;
    engine_detail::push_sync_entry(entries, record);
  }
  for (const auto& [id, record] : image.pending_publications) {
    (void)id;
    JournalEntry entry;
    entry.kind = JournalEntryKind::ClearPendingPublication;
    entry.remove_object = record.object;
    entries.push_back(std::move(entry));
  }
  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) {
    report.notes.push_back("recovery state could not be written back durably: " +
                           written.to_string());
    impl_->store_healthy = false;
  }
  impl_->compact_locked();

  impl_->recovery_performed = true;
  impl_->tick();
  impl_->recovery_report = report;
  report.notes.push_back("coordinator epoch advanced from " + std::to_string(previous) + " to " +
                         impl_->epoch.to_string());
  report.notes.push_back("every pre-restart session, boot incarnation and authority grant was "
                         "invalidated; dynamic currentness was not restored");

  const DecisionContext context =
      impl_->make_context(AuthorityContext{}, ObjectId::nil(), ObjectGeneration::nil(),
                          OwnershipGeneration::nil());
  impl_->record_decision(DecisionKind::Recover, context, StatusCode::Ok,
                         "durable recovery completed under epoch " + impl_->epoch.to_string());
  impl_->recovery_report = report;
  return Result<RecoveryReport>::success(report);
}

// ---------------------------------------------------------------------------
// Invariant auditor
// ---------------------------------------------------------------------------
AuditReport CoherenceEngine::audit() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  AuditReport report;
  report.epoch = impl_->epoch;
  report.sequence = impl_->sequence;
  report.participants_checked = impl_->participants.size();
  report.regions_checked = impl_->regions.size();
  report.invalidations_checked = impl_->invalidations.size();
  report.syncs_checked = impl_->syncs.size();
  report.objects_checked = impl_->objects.size();

  auto add = [&report](AuditFinding f) { report.findings.push_back(std::move(f)); };

  // --- identity uniqueness and index agreement -----------------------------
  {
    std::set<std::string> domain_names;
    for (const auto& [id, record] : impl_->domains) {
      if (record.id != id) {
        add(finding(StatusCode::InternalInvariantViolation, "domain map key disagrees with record id"));
      }
      if (!domain_names.insert(record.name).second) {
        add(finding(StatusCode::DuplicateIdentity, "duplicate domain name " + record.name));
      }
      const auto index = impl_->domain_index.find(record.name);
      if (index == impl_->domain_index.end() || index->second != record.id) {
        add(finding(StatusCode::InternalInvariantViolation,
                    "domain name index disagrees with the domain record for " + record.name));
      }
    }
    std::set<std::string> participant_names;
    for (const auto& [id, record] : impl_->participants) {
      if (record.id != id) {
        add(finding(StatusCode::InternalInvariantViolation,
                    "participant map key disagrees with record id"));
      }
      if (!participant_names.insert(record.name).second) {
        add(finding(StatusCode::DuplicateIdentity, "duplicate participant name " + record.name));
      }
      const auto index = impl_->participant_index.find(record.name);
      if (index == impl_->participant_index.end() || index->second != record.id) {
        add(finding(StatusCode::InternalInvariantViolation,
                    "participant name index disagrees for " + record.name));
      }
      if (record.lifecycle == ParticipantLifecycle::Fenced && record.live) {
        add(finding(StatusCode::InternalInvariantViolation,
                    "a fenced participant is marked live: " + record.name));
      }
      if (record.lifecycle == ParticipantLifecycle::Retired) {
        for (const auto& [oid, object] : impl_->objects) {
          (void)oid;
          if (object.writer == record.id) {
            add(finding(StatusCode::Retired,
                        "a retired participant retains write authority over " + object.name));
          }
        }
      }
    }
    for (const auto& [key, id] : impl_->object_index) {
      const auto it = impl_->objects.find(id);
      if (it == impl_->objects.end()) {
        add(finding(StatusCode::UnknownObject, "object name index references a missing object"));
        continue;
      }
      if (it->second.domain != key.domain || it->second.name != key.name) {
        add(finding(StatusCode::InternalInvariantViolation,
                    "object name index disagrees with the object record for " + key.name));
      }
    }
    for (const auto& [key, id] : impl_->region_index) {
      const auto it = impl_->regions.find(id);
      if (it == impl_->regions.end()) {
        add(finding(StatusCode::UnknownRegion, "region name index references a missing region"));
        continue;
      }
      if (it->second.object != key.object || it->second.name != key.name) {
        add(finding(StatusCode::InternalInvariantViolation,
                    "region name index disagrees with the region record for " + key.name));
      }
    }
  }

  // --- store health --------------------------------------------------------
  if (impl_->config.enable_durability && impl_->store != nullptr && !impl_->store_healthy) {
    add(finding(StatusCode::PersistenceFailure,
                "the durable store reported a failure since the last successful append"));
  }

  // --- participants / regions ---------------------------------------------
  std::map<ParticipantId, std::uint64_t> region_counts;
  for (const auto& [id, region] : impl_->regions) {
    (void)id;
    region_counts[region.participant] += 1;
    const auto object_it = impl_->objects.find(region.object);
    if (object_it == impl_->objects.end()) {
      add(finding(StatusCode::UnknownObject, "region references a missing object"));
      continue;
    }
    const ObjectRecord& object = object_it->second;
    AuditFinding f;
    f.object = object.id;
    f.region = region.id;

    if (region.object_generation != object.generation) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "region binds to object generation " + region.object_generation.to_string() +
                 " but the object generation is " + object.generation.to_string();
      add(f);
    }
    if (!checked_range(region.offset, region.length, object.length)) {
      f.code = StatusCode::InvalidArgument;
      f.detail = "region extent lies outside the object extent";
      add(f);
    }
    const auto participant_it = impl_->participants.find(region.participant);
    if (participant_it == impl_->participants.end()) {
      f.code = StatusCode::UnknownParticipant;
      f.detail = "region references a missing participant";
      add(f);
    } else {
      const ParticipantRecord& participant = participant_it->second;
      const bool terminal = region.lifecycle == RegionLifecycle::Retired ||
                            region.state == CoherenceState::Fenced;
      if (!terminal && participant.boot != region.boot) {
        f.code = StatusCode::StaleBoot;
        f.detail = "a live region carries a boot identity that is not the participant's live boot";
        add(f);
      }
      if (!terminal && participant.lifecycle == ParticipantLifecycle::Fenced) {
        f.code = StatusCode::Fenced;
        f.detail = "a region of a fenced participant is not marked fenced";
        add(f);
      }
    }
    if (region.lifecycle == RegionLifecycle::Retired &&
        (region.state != CoherenceState::Retired)) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "a retired region is not in the retired coherence state";
      add(f);
    }
    if (region.state == CoherenceState::Retired && region.invalidation_pending) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "a retired replica still has an outstanding invalidation";
      add(f);
    }
    if (region.state == CoherenceState::Current && !region.evidence.defined()) {
      f.code = StatusCode::EvidenceMissing;
      f.detail = "a replica is marked current without any supporting evidence";
      add(f);
    }
    if (region.state == CoherenceState::Current && object.authoritative_version.defined() &&
        region.version != object.authoritative_version) {
      f.code = StatusCode::ReadNotCurrent;
      f.detail = "a current replica does not match the authoritative version";
      add(f);
    }
    if (region.state == CoherenceState::Dirty && region.dirty != DirtyCondition::DirtyUnpublished) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "a dirty replica does not carry an unpublished modification";
      add(f);
    }
    if (region.state == CoherenceState::Dirty && !region.declared_writable) {
      f.code = StatusCode::InvalidTransition;
      f.detail = "a replica declared read-only is marked dirty";
      add(f);
    }
    if (region.invalidation_pending) {
      if (!region.pending_invalidation.defined()) {
        f.code = StatusCode::InternalInvariantViolation;
        f.detail = "a pending invalidation is not identified";
        add(f);
      } else {
        const auto invalidation = impl_->invalidations.find(region.pending_invalidation);
        if (invalidation == impl_->invalidations.end()) {
          f.code = StatusCode::UnknownInvalidation;
          f.detail = "a pending invalidation references a missing record";
          add(f);
        } else if (invalidation->second.state != InvalidationState::Requested) {
          f.code = StatusCode::InternalInvariantViolation;
          f.detail = "a settled invalidation is still marked pending on the replica";
          add(f);
        }
      }
    }
    if (object.authoritative_version.defined() && region.version.defined() &&
        region.version > object.authoritative_version) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "a replica claims a version newer than the authoritative version";
      add(f);
    }
    if (object.ownership_generation.defined() && region.ownership_generation.defined() &&
        region.ownership_generation > object.ownership_generation) {
      f.code = StatusCode::StaleOwnership;
      f.detail = "a replica carries an ownership generation from the future";
      add(f);
    }
  }
  for (const auto& [id, participant] : impl_->participants) {
    const std::uint64_t actual = region_counts.count(id) != 0 ? region_counts.at(id) : 0;
    if (participant.region_count != actual) {
      add(finding(StatusCode::InternalInvariantViolation,
                  "participant region accounting does not close: " + participant.name));
    }
  }

  // --- objects -------------------------------------------------------------
  std::size_t exclusive_writers = 0;
  for (const auto& [id, object] : impl_->objects) {
    (void)id;
    AuditFinding f;
    f.object = object.id;
    if (!impl_->domains.count(object.domain)) {
      f.code = StatusCode::UnknownDomain;
      f.detail = "object references a missing domain";
      add(f);
    }
    if (!impl_->policies.count(object.policy)) {
      f.code = StatusCode::UnknownPolicy;
      f.detail = "object references a missing policy";
      add(f);
    }
    std::vector<RegionId> replicas = object.replicas;
    std::sort(replicas.begin(), replicas.end());
    if (std::adjacent_find(replicas.begin(), replicas.end()) != replicas.end()) {
      f.code = StatusCode::DuplicateIdentity;
      f.detail = "object lists the same replica more than once";
      add(f);
    }
    if (replicas != object.replicas) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "object replica list is not in canonical order";
      add(f);
    }
    for (const RegionId region_id : object.replicas) {
      const auto region = impl_->regions.find(region_id);
      if (region == impl_->regions.end()) {
        f.code = StatusCode::UnknownRegion;
        f.detail = "object references a missing replica";
        add(f);
      } else if (region->second.object != object.id) {
        f.code = StatusCode::InternalInvariantViolation;
        f.detail = "a replica listed by this object belongs to another object";
        add(f);
      }
    }
    if (object.authoritative_version.defined() && !object.publication.defined()) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "an object has an authoritative version but no publication identity";
      add(f);
    }
    if (object.published_version.defined() && object.authoritative_version.defined() &&
        object.published_version > object.authoritative_version) {
      f.code = StatusCode::StalePublication;
      f.detail = "the published version is newer than the authoritative version";
      add(f);
    }
    if (object.authority == AuthorityMode::ExclusiveWriter ||
        object.authority == AuthorityMode::TransferPending) {
      ++exclusive_writers;
      if (!object.writer.defined()) {
        f.code = StatusCode::NotAuthoritative;
        f.detail = "write authority is recorded without an owner";
        add(f);
      } else {
        const auto participant = impl_->participants.find(object.writer);
        if (participant == impl_->participants.end()) {
          f.code = StatusCode::UnknownParticipant;
          f.detail = "write authority references a missing participant";
          add(f);
        } else {
          if (participant->second.boot != object.writer_boot) {
            f.code = StatusCode::StaleBoot;
            f.detail = "write authority is bound to a boot identity that is no longer live";
            add(f);
          }
          if (participant->second.lifecycle == ParticipantLifecycle::Fenced ||
              participant->second.lifecycle == ParticipantLifecycle::Retired) {
            f.code = StatusCode::Fenced;
            f.detail = "a fenced or retired participant retains write authority";
            add(f);
          }
        }
      }
    }
    if (object.authority == AuthorityMode::TransferPending &&
        !impl_->has_outstanding_invalidations_locked(object.id)) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "a write transfer is pending with no outstanding invalidation";
      add(f);
    }
    std::size_t live_reads = 0;
    for (const ReadGrant& grant : object.reads) {
      if (grant.released) continue;
      ++live_reads;
      const auto region = impl_->regions.find(grant.region);
      if (region == impl_->regions.end()) {
        f.code = StatusCode::UnknownRegion;
        f.detail = "a live read grant references a missing replica";
        add(f);
      } else if (!is_current_state(region->second.state) &&
                 !is_readable_state(region->second.state)) {
        f.code = StatusCode::ReadNotCurrent;
        f.detail = "a live read grant references a replica that cannot be read";
        add(f);
      }
      if (object.has_unpublished_dirty && grant.version != object.authoritative_version &&
          grant.version.defined()) {
        f.code = StatusCode::DirtyUnpublished;
        f.detail = "a reader holds a version while an unpublished modification exists";
        add(f);
      }
    }
    if (object.authority == AuthorityMode::ExclusiveWriter ||
        object.authority == AuthorityMode::TransferPending) {
      for (const ReadGrant& grant : object.reads) {
        if (grant.released) continue;
        if (grant.participant == object.writer && grant.boot == object.writer_boot) continue;
        f.code = StatusCode::ExclusiveWriterConflict;
        f.detail =
            "exclusive write authority coexists with a live read grant held by another "
            "participant incarnation";
        add(f);
        break;
      }
    }
    if (object.has_unpublished_dirty && object.dirty_condition != DirtyCondition::DirtyUnpublished) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "an unpublished modification is tracked without the matching dirty condition";
      add(f);
    }
    if (object.publication_state == PublicationState::PendingDurable) {
      if (!object.pending_publication.defined()) {
        f.code = StatusCode::InternalInvariantViolation;
        f.detail = "a publication is pending durability without a publication identity";
        add(f);
      }
      if (impl_->config.enable_durability && impl_->store != nullptr &&
          !impl_->durable.pending_publications.count(object.pending_publication) &&
          impl_->in_flight_publications.count(object.pending_publication) == 0) {
        f.code = StatusCode::PersistenceFailure;
        f.detail = "a publication is pending durability with no durable pending record";
        add(f);
      }
    }
    for (const InvalidationId invalidation_id : object.outstanding_invalidations) {
      const auto invalidation = impl_->invalidations.find(invalidation_id);
      if (invalidation == impl_->invalidations.end()) {
        f.code = StatusCode::UnknownInvalidation;
        f.detail = "an outstanding invalidation record is missing";
        add(f);
      } else if (invalidation->second.state != InvalidationState::Requested) {
        f.code = StatusCode::InternalInvariantViolation;
        f.detail = "a settled invalidation is still listed as outstanding";
        add(f);
      }
    }
  }
  (void)exclusive_writers;  // one writer per object generation is structural

  // --- invalidations -------------------------------------------------------
  for (const auto& [id, invalidation] : impl_->invalidations) {
    (void)id;
    AuditFinding f;
    f.object = invalidation.object;
    f.region = invalidation.region;
    f.invalidation = invalidation.id;
    if (!impl_->objects.count(invalidation.object)) {
      f.code = StatusCode::UnknownObject;
      f.detail = "invalidation references a missing object";
      add(f);
    }
    if (invalidation.epoch.defined() && invalidation.epoch > impl_->epoch) {
      f.code = StatusCode::StaleEpoch;
      f.detail = "invalidation carries an epoch from the future";
      add(f);
    }
    if (invalidation.state == InvalidationState::Acknowledged &&
        !invalidation.settled_sequence.defined()) {
      f.code = StatusCode::InternalInvariantViolation;
      f.detail = "an acknowledged invalidation has no settle sequence";
      add(f);
    }
    if (invalidation.target_boot.defined()) {
      const auto participant = impl_->participants.find(invalidation.target_participant);
      if (participant != impl_->participants.end() &&
          invalidation.state == InvalidationState::Requested &&
          participant->second.boot != invalidation.target_boot) {
        f.code = StatusCode::StaleBoot;
        f.detail = "a requested invalidation targets a boot identity that is no longer live";
        add(f);
      }
    }
  }

  // --- synchronizations ----------------------------------------------------
  for (const auto& [id, sync] : impl_->syncs) {
    (void)id;
    AuditFinding f;
    f.object = sync.object;
    f.sync = sync.id;
    if (!impl_->objects.count(sync.object)) {
      f.code = StatusCode::UnknownObject;
      f.detail = "synchronization references a missing object";
      add(f);
    }
    if (!impl_->regions.count(sync.source_region)) {
      f.code = StatusCode::UnknownRegion;
      f.detail = "synchronization references a missing source region";
      add(f);
    }
    if (!impl_->regions.count(sync.destination_region)) {
      f.code = StatusCode::UnknownRegion;
      f.detail = "synchronization references a missing destination region";
      add(f);
    }
    if (sync.epoch.defined() && sync.epoch > impl_->epoch) {
      f.code = StatusCode::StaleEpoch;
      f.detail = "synchronization carries an epoch from the future";
      add(f);
    }
    if (sync.state == SyncState::Completed &&
        sync.destination_region_generation !=
            (impl_->regions.count(sync.destination_region)
                 ? impl_->regions.at(sync.destination_region).generation
                 : RegionGeneration::nil())) {
      f.code = StatusCode::StaleRegionGeneration;
      f.detail = "a completed synchronization targets a region generation that no longer exists";
      add(f);
    }
  }

  std::stable_sort(report.findings.begin(), report.findings.end(),
                   [](const AuditFinding& a, const AuditFinding& b) {
                     if (a.code != b.code) return a.code < b.code;
                     if (a.object != b.object) return a.object < b.object;
                     if (a.region != b.region) return a.region < b.region;
                     if (a.invalidation != b.invalidation) return a.invalidation < b.invalidation;
                     if (a.sync != b.sync) return a.sync < b.sync;
                     return a.detail < b.detail;
                   });
  report.clean = report.findings.empty();
  return report;
}

// ---------------------------------------------------------------------------
// Deterministic renderings
// ---------------------------------------------------------------------------
std::string CoherenceSnapshot::render() const {
  std::string out;
  out.reserve(4096);
  out.append("coherence_snapshot build=\"");
  out.append(build);
  out.append("\" epoch=");
  out.append(epoch.to_string());
  out.append(" sequence=");
  out.append(sequence.to_string());
  out.append(" shutting_down=");
  out.append(shutting_down ? "true" : "false");
  out.push_back('\n');
  out.append("domains=");
  out.append(std::to_string(domains.size()));
  out.append(" participants=");
  out.append(std::to_string(participants.size()));
  out.append(" objects=");
  out.append(std::to_string(objects.size()));
  out.append(" regions=");
  out.append(std::to_string(regions.size()));
  out.append(" invalidations=");
  out.append(std::to_string(invalidations.size()));
  out.append(" syncs=");
  out.append(std::to_string(syncs.size()));
  out.push_back('\n');
  for (const DomainRecord& domain : domains) {
    out.append("  domain ");
    out.append(domain.id.to_string());
    out.append(" name=");
    out.append(domain.name);
    out.append(" generation=");
    out.append(std::to_string(domain.generation));
    out.append(" lifecycle=");
    out.append(to_token(domain.lifecycle));
    out.append(" created_epoch=");
    out.append(domain.created_epoch.to_string());
    out.append(" current_epoch=");
    out.append(domain.current_epoch.to_string());
    out.push_back('\n');
  }
  for (const ParticipantRecord& participant : participants) {
    out.append("  ");
    out.append(render_participant(participant));
    out.push_back('\n');
  }
  for (const ObjectRecord& object : objects) {
    out.append("  ");
    out.append(render_object(object));
    out.push_back('\n');
  }
  for (const RegionRecord& region : regions) {
    out.append("  ");
    out.append(render_region(region));
    out.push_back('\n');
  }
  for (const InvalidationRecord& invalidation : invalidations) {
    out.append("  ");
    out.append(render_invalidation(invalidation));
    out.push_back('\n');
  }
  for (const SyncRecord& sync : syncs) {
    out.append("  ");
    out.append(render_sync(sync));
    out.push_back('\n');
  }
  return out;
}

std::string AuditReport::render() const {
  std::string out;
  out.reserve(1024);
  out.append("invariant_audit result=");
  out.append(clean ? "clean" : "violations");
  out.append(" violations=");
  out.append(std::to_string(findings.size()));
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" sequence=");
  out.append(sequence.to_string());
  out.push_back('\n');
  out.append("  checked objects=");
  out.append(std::to_string(objects_checked));
  out.append(" regions=");
  out.append(std::to_string(regions_checked));
  out.append(" participants=");
  out.append(std::to_string(participants_checked));
  out.append(" invalidations=");
  out.append(std::to_string(invalidations_checked));
  out.append(" synchronizations=");
  out.append(std::to_string(syncs_checked));
  out.push_back('\n');
  for (const AuditFinding& f : findings) {
    out.append("  violation code=");
    out.append(status_code_name(f.code));
    out.append(" object=");
    out.append(f.object.to_string());
    out.append(" region=");
    out.append(f.region.to_string());
    out.append(" participant=");
    out.append(f.participant.to_string());
    out.append(" detail=");
    out.append(f.detail);
    out.push_back('\n');
  }
  return out;
}

std::string RecoveryReport::render() const {
  std::string out;
  out.reserve(1024);
  out.append("recovery performed=");
  out.append(performed ? "true" : "false");
  out.append(" previous_epoch=");
  out.append(previous_epoch.to_string());
  out.append(" new_epoch=");
  out.append(new_epoch.to_string());
  out.push_back('\n');
  out.append("  domains_restored=");
  out.append(std::to_string(objects_restored));
  out.append(" participants_restored=");
  out.append(std::to_string(participants_restored));
  out.append(" regions_restored=");
  out.append(std::to_string(regions_restored));
  out.push_back('\n');
  out.append("  regions_downgraded=");
  out.append(std::to_string(regions_downgraded));
  out.append(" authority_revoked=");
  out.append(std::to_string(authority_revoked));
  out.append(" publications_completed=");
  out.append(std::to_string(publications_completed));
  out.append(" publications_requiring_operator=");
  out.append(std::to_string(publications_requiring_operator));
  out.push_back('\n');
  out.append("  syncs_classified_unknown=");
  out.append(std::to_string(syncs_classified_unknown));
  out.append(" invalidations_superseded=");
  out.append(std::to_string(invalidations_superseded));
  out.append(" journal_records_replayed=");
  out.append(std::to_string(journal_records_replayed));
  out.append(" corrupt_records_skipped=");
  out.append(std::to_string(corrupt_records_skipped));
  out.push_back('\n');
  for (const std::string& note : notes) {
    out.append("  note=");
    out.append(note);
    out.push_back('\n');
  }
  return out;
}

} // namespace coherence
