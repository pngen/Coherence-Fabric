// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Publication, release and synchronization.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "coherence/engine.hpp"
#include "engine_helpers.hpp"
#include "engine_impl.hpp"

namespace coherence {

using engine_detail::policy_locked;
using engine_detail::push_object_entry;
using engine_detail::push_region_entry;
using engine_detail::push_sync_entry;
using engine_detail::Resolved;
using engine_detail::resolve_locked;
using engine_detail::resolve_region_locked;

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------
Result<PublicationReceipt> CoherenceEngine::publish(const PublishRequest& request) {
  PublicationReceipt receipt;
  PendingPublication pending;
  JournalEntry pending_entry;
  VersionId base;
  OwnershipGeneration ownership;
  ParticipantId writer;
  ParticipantBootId writer_boot;
  bool durable_required = false;

  // ---- Phase 1: validate and reserve, under the state lock. ----------------
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    Resolved resolved;
    Status authority = resolve_locked(*impl_, request.context, resolved);
    const DecisionContext context = impl_->make_context(
        request.context, request.context.object, request.context.object_generation,
        resolved.object != nullptr ? resolved.object->ownership_generation
                                   : OwnershipGeneration::nil());
    receipt.context = context;
    if (!authority.ok()) {
      receipt.state = PublicationState::None;
      receipt.reason = authority.code();
      receipt.rationale = authority.to_string();
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::failure(authority);
    }
    ObjectRecord& object = *resolved.object;
    const CoherencePolicy* policy = policy_locked(*impl_, object);
    if (policy == nullptr) {
      receipt.reason = StatusCode::UnknownPolicy;
      receipt.rationale = "the object references a policy that is not present";
      return Result<PublicationReceipt>::failure(Status(receipt.reason, receipt.rationale));
    }
    receipt.durability = policy->publication_durability;
    writer = request.context.participant;
    writer_boot = resolved.participant->boot;
    base = object.authoritative_version;
    receipt.prior_version = base;
    receipt.writer = writer;
    receipt.writer_boot = writer_boot;
    receipt.content = request.content;

    // Idempotent retry: a request identity that already committed returns the
    // receipt of the original publication instead of creating a new version.
    // The record is durable, so the guarantee survives a coordinator restart.
    if (request.context.request.defined()) {
      for (const CompletedPublicationRequest& completed : object.completed_requests) {
        if (completed.request != request.context.request) continue;
        receipt.publication = completed.publication;
        receipt.version = completed.version;
        receipt.state = PublicationState::Committed;
        receipt.reason = StatusCode::Ok;
        receipt.durable = policy->publication_durability == PublicationDurability::DurableMetadata;
        receipt.idempotent_replay = true;
        receipt.rationale =
            "this request identity already committed; the original publication and version are "
            "returned and no second version is created";
        impl_->record_decision(DecisionKind::Publish, context, StatusCode::Ok, receipt.rationale);
        return Result<PublicationReceipt>::success(receipt);
      }
    }

    if (object.authority != AuthorityMode::ExclusiveWriter) {
      receipt.state = PublicationState::None;
      receipt.reason = object.authority == AuthorityMode::TransferPending
                           ? StatusCode::InvalidationOutstanding
                           : StatusCode::NotWriteAuthorized;
      receipt.rationale =
          "the caller does not hold exclusive write authority for this object generation";
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }
    if (object.writer != request.context.participant ||
        object.writer_boot != request.context.boot) {
      receipt.state = PublicationState::None;
      receipt.reason = StatusCode::NotWriteAuthorized;
      receipt.rationale = "the caller is not the holder of write authority for this object";
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }
    if (request.ownership_generation != object.ownership_generation) {
      receipt.state = PublicationState::None;
      receipt.reason = StatusCode::StaleOwnership;
      receipt.rationale = "publication carries a stale ownership generation";
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }
    if (request.expected_base_version != base) {
      receipt.state = PublicationState::None;
      receipt.reason = StatusCode::StalePublication;
      receipt.rationale = "publication carries a stale base version";
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }
    if (policy->require_invalidation_acks_before_publication &&
        impl_->has_outstanding_invalidations_locked(object.id)) {
      receipt.state = PublicationState::None;
      receipt.reason = StatusCode::InvalidationOutstanding;
      receipt.rationale =
          "a mandatory invalidation has not been acknowledged; publication may not commit";
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }
    if (policy->require_content_fingerprint && !request.content.defined) {
      receipt.state = PublicationState::None;
      receipt.reason = StatusCode::EvidenceMissing;
      receipt.rationale =
          "the active policy requires a content fingerprint for publication and none was supplied";
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }

    RegionRecord* region = nullptr;
    Status resolved_region = resolve_region_locked(*impl_, resolved, request.region,
                                                   request.region_generation, region);
    if (!resolved_region.ok()) {
      receipt.state = PublicationState::None;
      receipt.reason = resolved_region.code();
      receipt.rationale = resolved_region.to_string();
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }
    if (region->participant != request.context.participant) {
      receipt.state = PublicationState::None;
      receipt.reason = StatusCode::NotWriteAuthorized;
      receipt.rationale = "the caller does not own the region being published";
      impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
      return Result<PublicationReceipt>::success(receipt);
    }
    if (!request.allow_without_dirty) {
      if (region->dirty != DirtyCondition::DirtyUnpublished) {
        receipt.state = PublicationState::None;
        receipt.reason = StatusCode::DirtyUnpublished;
        receipt.rationale =
            "publication without a dirty replica is only permitted for the explicit initial "
            "establishment path";
        impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
        return Result<PublicationReceipt>::success(receipt);
      }
      if (region->dirty_base_version != base) {
        receipt.state = PublicationState::None;
        receipt.reason = StatusCode::StalePublication;
        receipt.rationale = "the dirty replica diverged from a different base version";
        impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
        return Result<PublicationReceipt>::success(receipt);
      }
    } else {
      if (region->state != CoherenceState::Current && region->state != CoherenceState::Dirty) {
        receipt.state = PublicationState::None;
        receipt.reason = StatusCode::SyncRequired;
        receipt.rationale = "initial establishment requires a replica that is current or dirty";
        impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
        return Result<PublicationReceipt>::success(receipt);
      }
      if (base.defined() && region->version != base) {
        receipt.state = PublicationState::None;
        receipt.reason = StatusCode::StalePublication;
        receipt.rationale =
            "initial establishment is only permitted before an authoritative version exists";
        impl_->record_decision(DecisionKind::Publish, context, receipt.reason, receipt.rationale);
        return Result<PublicationReceipt>::success(receipt);
      }
    }

    impl_->tick();
    const VersionId version = base.defined() ? base.next() : VersionId::from_value(1);
    const PublicationId publication = PublicationId::from_value(impl_->next_publication_id++);
    ownership = object.ownership_generation;
    durable_required = policy->publication_durability == PublicationDurability::DurableMetadata &&
                       impl_->config.enable_durability && impl_->store != nullptr;

    pending.id = publication;
    pending.object = object.id;
    pending.object_generation = object.generation;
    pending.version = version;
    pending.prior_version = base;
    pending.ownership_generation = ownership;
    pending.epoch = impl_->epoch;
    pending.writer = writer;
    pending.writer_boot = writer_boot;
    pending.request = request.context.request;
    pending.content = request.content;
    pending.sequence = impl_->sequence;

    object.publication_state = PublicationState::PendingDurable;
    object.pending_publication = publication;
    object.pending_version = version;
    object.pending_content = request.content;
    object.updated_sequence = impl_->sequence;
    impl_->in_flight_publications[publication] = pending;

    // A fixed journal slot is reserved now so the append can be performed
    // without holding the state lock.
    pending_entry.kind = JournalEntryKind::SetPendingPublication;
    pending_entry.pending = pending;
    pending_entry.sequence = impl_->next_journal_sequence++;

    receipt.publication = publication;
    receipt.version = version;
  }

  // ---- Phase 2: durability, with the state lock released. -----------------
  Status durable_status = Status::success();
  if (durable_required) {
    std::lock_guard<std::mutex> store_guard(impl_->store_mutex);
    durable_status = impl_->store->append(pending_entry);
    if (!durable_status.ok()) impl_->store_healthy = false;
  }

  // ---- Phase 3: re-validate the reservation and commit visibility. --------
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->in_flight_publications.erase(pending.id);
  const auto object_it = impl_->objects.find(pending.object);
  if (object_it == impl_->objects.end()) {
    receipt.state = PublicationState::Aborted;
    receipt.reason = StatusCode::UnknownObject;
    receipt.rationale = "the object disappeared while the publication was being made durable";
    return Result<PublicationReceipt>::failure(Status(receipt.reason, receipt.rationale));
  }
  ObjectRecord& object = object_it->second;
  if (!durable_status.ok()) {
    object.publication_state = PublicationState::Aborted;
    object.pending_publication = PublicationId::nil();
    object.pending_version = VersionId::nil();
    object.pending_content = ContentFingerprint{};
    receipt.state = PublicationState::Aborted;
    receipt.reason = durable_status.code();
    receipt.rationale =
        "publication aborted because its durable record could not be committed; no version became "
        "authoritative";
    impl_->record_decision(DecisionKind::Publish, receipt.context, receipt.reason,
                           receipt.rationale);
    return Result<PublicationReceipt>::failure(durable_status);
  }
  if (durable_required) {
    Status applied = apply_journal_entry(impl_->durable, pending_entry, impl_->store->limits());
    if (!applied.ok()) impl_->store_healthy = false;
  }
  if (object.pending_publication != pending.id || object.ownership_generation != ownership ||
      object.authoritative_version != base) {
    object.publication_state = PublicationState::Aborted;
    object.pending_publication = PublicationId::nil();
    object.pending_version = VersionId::nil();
    object.pending_content = ContentFingerprint{};
    if (durable_required) {
      std::lock_guard<std::mutex> store_guard(impl_->store_mutex);
      JournalEntry clear;
      clear.kind = JournalEntryKind::ClearPendingPublication;
      clear.remove_object = object.id;
      clear.sequence = impl_->next_journal_sequence++;
      (void)impl_->store->append(clear);
      (void)apply_journal_entry(impl_->durable, clear, impl_->store->limits());
    }
    receipt.state = PublicationState::Aborted;
    receipt.reason = StatusCode::OutcomeUnknown;
    receipt.rationale =
        "the publication reservation was superseded while its durable record was being written; no "
        "version became authoritative";
    impl_->record_decision(DecisionKind::Publish, receipt.context, receipt.reason,
                           receipt.rationale);
    return Result<PublicationReceipt>::success(receipt);
  }

  impl_->tick();
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
  object.last_writer = writer;
  object.last_writer_boot = writer_boot;
  object.updated_sequence = impl_->sequence;
  object.recovery_note.clear();

  const std::size_t max_requests = static_cast<std::size_t>(
      impl_->config.max_retained_requests_per_object == 0
          ? 1
          : impl_->config.max_retained_requests_per_object);
  if (request.context.request.defined()) {
    object.completed_requests.push_back(
        CompletedPublicationRequest{request.context.request, pending.version, pending.id,
                                    impl_->sequence});
    while (object.completed_requests.size() > max_requests) {
      object.completed_requests.erase(object.completed_requests.begin());
    }
  }

  const auto region_it = impl_->regions.find(request.region);
  if (region_it != impl_->regions.end()) {
    RegionRecord& region = region_it->second;
    const EvidenceRecord evidence = impl_->make_evidence(
        EvidenceKind::ByteComparison, region.evidence_class, writer, writer_boot, object.id,
        object.generation, region.id, region.generation, pending.version, request.content);
    region.state = CoherenceState::Current;
    region.version = pending.version;
    region.last_published_version = pending.version;
    region.ownership_generation = ownership;
    region.dirty = DirtyCondition::DirtyPublished;
    region.dirty_published = true;
    region.evidence = evidence.id;
    region.evidence_generation = evidence.generation;
    if (request.content.defined) region.content = request.content;
    region.updated_sequence = impl_->sequence;
    region.note = "authoritative version " + pending.version.to_string() +
                  " published from this replica";
  }

  impl_->mark_regions_stale_locked(object.id, pending.version, request.region);

  std::vector<JournalEntry> entries;
  push_object_entry(entries, object);
  {
    JournalEntry clear;
    clear.kind = JournalEntryKind::ClearPendingPublication;
    clear.remove_object = object.id;
    entries.push_back(std::move(clear));
  }
  if (region_it != impl_->regions.end()) push_region_entry(entries, region_it->second);
  for (const RegionId id : object.replicas) {
    if (id == request.region) continue;
    const auto it = impl_->regions.find(id);
    if (it == impl_->regions.end()) continue;
    push_region_entry(entries, it->second);
  }
  Status commit_written = impl_->append_many_locked(entries);
  if (!commit_written.ok()) {
    receipt.rationale =
        "publication committed; the commit marker could not be appended, but the durable pending "
        "record fixes the exact version so recovery converges on the same outcome";
  } else {
    receipt.rationale =
        "publication committed as the authoritative version after its durable record was written";
  }

  receipt.state = PublicationState::Committed;
  receipt.reason = StatusCode::Ok;
  receipt.durable = durable_required;
  impl_->record_decision(DecisionKind::Publish, receipt.context, StatusCode::Ok,
                         receipt.rationale);
  (void)impl_->maybe_compact_locked();
  return Result<PublicationReceipt>::success(receipt);
}

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------
Result<ReleaseOutcome> CoherenceEngine::release(const ReleaseRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Resolved resolved;
  ReleaseOutcome outcome;
  Status authority = resolve_locked(*impl_, request.context, resolved);
  if (resolved.object != nullptr) {
    outcome.context = impl_->make_context(request.context, request.context.object,
                                          request.context.object_generation,
                                          resolved.object->ownership_generation);
  }
  outcome.participant = request.context.participant;
  outcome.boot = request.context.boot;
  if (!authority.ok()) {
    outcome.reason = authority.code();
    outcome.rationale = authority.to_string();
    return Result<ReleaseOutcome>::failure(authority);
  }
  ObjectRecord& object = *resolved.object;

  if (request.lease.defined()) {
    outcome.lease = request.lease;
    bool found = false;
    for (ReadGrant& grant : object.reads) {
      if (grant.lease != request.lease) continue;
      found = true;
      if (grant.participant != request.context.participant ||
          grant.boot != request.context.boot) {
        const Status failure =
            Status(StatusCode::NotWriteAuthorized,
                   "the read lease belongs to a different participant incarnation",
                   request.lease.to_string());
        outcome.reason = failure.code();
        outcome.rationale = failure.to_string();
        return Result<ReleaseOutcome>::failure(failure);
      }
      outcome.already_released = grant.released;
      grant.released = true;
      outcome.version = grant.version;
      break;
    }
    if (!found) {
      const Status failure = Status(StatusCode::UnknownRequest, "no such read lease",
                                    request.lease.to_string());
      outcome.reason = failure.code();
      outcome.rationale = failure.to_string();
      return Result<ReleaseOutcome>::failure(failure);
    }
    impl_->recompute_authority_locked(object);
    object.updated_sequence = impl_->sequence;
    std::vector<JournalEntry> entries;
    push_object_entry(entries, object);
    Status written = impl_->append_many_locked(entries);
    if (!written.ok()) return Result<ReleaseOutcome>::failure(written);
    impl_->tick();
    outcome.reason = StatusCode::Ok;
    outcome.rationale = outcome.already_released
                            ? "the read lease had already been released; the release is idempotent"
                            : "read lease released";
    impl_->record_decision(DecisionKind::Release, outcome.context, StatusCode::Ok,
                           outcome.rationale);
    return Result<ReleaseOutcome>::success(outcome);
  }

  if (!request.release_write_authority) {
    const Status failure = Status(StatusCode::InvalidArgument,
                                  "release must name either a read lease or write authority");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<ReleaseOutcome>::failure(failure);
  }

  if (object.writer != request.context.participant ||
      object.writer_boot != request.context.boot) {
    const Status failure = Status(StatusCode::NotWriteAuthorized,
                                  "the caller does not hold write authority for this object");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<ReleaseOutcome>::failure(failure);
  }
  if (request.ownership_generation != object.ownership_generation) {
    const Status failure = Status(StatusCode::StaleOwnership,
                                  "release carries a stale ownership generation");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<ReleaseOutcome>::failure(failure);
  }

  impl_->tick();
  std::vector<JournalEntry> entries;
  // Releasing write authority advances the ownership generation so a lingering
  // actor holding the previous generation can never write again.
  object.ownership_generation = object.ownership_generation.next();
  object.writer = ParticipantId::nil();
  object.writer_boot = ParticipantBootId::nil();
  object.authority = AuthorityMode::None;
  if (object.has_unpublished_dirty) {
    object.has_unpublished_dirty = false;
    const CoherencePolicy* policy = policy_locked(*impl_, object);
    switch (policy != nullptr ? policy->dirty_loss_policy : DirtyLossPolicy::ReportUnknown) {
      case DirtyLossPolicy::ReportLost:
        object.dirty_condition = DirtyCondition::DirtyLost;
        break;
      case DirtyLossPolicy::RequireRecovery:
        object.dirty_condition = DirtyCondition::DirtyUnknown;
        object.lifecycle = ObjectLifecycle::RecoveryRequired;
        object.recovery_note =
            "write authority released with an unpublished modification; the newest state was never "
            "published and recovery must resolve the outcome";
        break;
      case DirtyLossPolicy::ReportUnknown:
      case DirtyLossPolicy::Unspecified:
      default:
        object.dirty_condition = DirtyCondition::DirtyUnknown;
        break;
    }
    for (const RegionId id : object.replicas) {
      const auto it = impl_->regions.find(id);
      if (it == impl_->regions.end()) continue;
      if (it->second.dirty != DirtyCondition::DirtyUnpublished) continue;
      it->second.dirty = DirtyCondition::DirtyLost;
      it->second.state = CoherenceState::Stale;
      it->second.note = "unpublished modification was never published";
      it->second.updated_sequence = impl_->sequence;
      push_region_entry(entries, it->second);
    }
  }
  impl_->recompute_authority_locked(object);
  object.updated_sequence = impl_->sequence;
  push_object_entry(entries, object);
  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) return Result<ReleaseOutcome>::failure(written);
  outcome.reason = StatusCode::Ok;
  outcome.rationale = "write authority released and the ownership generation advanced";
  impl_->record_decision(DecisionKind::Release, outcome.context, StatusCode::Ok,
                         outcome.rationale);
  return Result<ReleaseOutcome>::success(outcome);
}

// ---------------------------------------------------------------------------
// Synchronization
// ---------------------------------------------------------------------------
Result<SyncOutcome> CoherenceEngine::begin_sync(const SyncRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Resolved resolved;
  Status authority = resolve_locked(*impl_, request.context, resolved);
  SyncOutcome outcome;
  if (resolved.object != nullptr) {
    outcome.context = impl_->make_context(request.context, request.context.object,
                                          request.context.object_generation,
                                          resolved.object->ownership_generation);
  }
  if (!authority.ok()) {
    outcome.reason = authority.code();
    outcome.rationale = authority.to_string();
    return Result<SyncOutcome>::failure(authority);
  }
  ObjectRecord& object = *resolved.object;
  const CoherencePolicy* policy = policy_locked(*impl_, object);
  if (policy == nullptr) {
    return Result<SyncOutcome>::failure(
        Status(StatusCode::UnknownPolicy, "the object references a policy that is not present"));
  }
  if (impl_->syncs.size() >= impl_->config.max_inflight_syncs) {
    return Result<SyncOutcome>::failure(Status(StatusCode::CapacityExceeded,
                                               "in-flight synchronization capacity reached"));
  }

  RegionRecord* destination = nullptr;
  Status resolved_destination =
      resolve_region_locked(*impl_, resolved, request.destination_region,
                            request.destination_region_generation, destination);
  if (!resolved_destination.ok()) {
    outcome.reason = resolved_destination.code();
    outcome.rationale = resolved_destination.to_string();
    return Result<SyncOutcome>::failure(resolved_destination);
  }
  if (destination->participant != request.context.participant ||
      destination->boot != request.context.boot) {
    const Status failure = Status(
        StatusCode::NotWriteAuthorized,
        "only the owning participant incarnation may synchronize a replica into its own region");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  if (destination->lifecycle == RegionLifecycle::Retired) {
    const Status failure = Status(StatusCode::Retired, "the destination replica is retired");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }

  // Source selection is deterministic: the lowest region identity that is
  // current at the authoritative version.
  RegionId source_id = request.source_region;
  if (source_id.defined()) {
    const auto it = impl_->regions.find(source_id);
    if (it == impl_->regions.end()) {
      const Status failure = Status(StatusCode::UnknownRegion, "no such source region",
                                    source_id.to_string());
      outcome.reason = failure.code();
      outcome.rationale = failure.to_string();
      return Result<SyncOutcome>::failure(failure);
    }
    if (request.source_region_generation.defined() &&
        it->second.generation != request.source_region_generation) {
      const Status failure = Status(
          StatusCode::StaleRegionGeneration,
          "the nominated source region generation is not the live one",
          "request=" + request.source_region_generation.to_string() +
              " live=" + it->second.generation.to_string());
      outcome.reason = failure.code();
      outcome.rationale = failure.to_string();
      return Result<SyncOutcome>::failure(failure);
    }
    if (it->second.object != object.id) {
      const Status failure = Status(StatusCode::InvalidArgument,
                                    "the source region does not belong to this object");
      outcome.reason = failure.code();
      outcome.rationale = failure.to_string();
      return Result<SyncOutcome>::failure(failure);
    }
    if (it->second.state != CoherenceState::Current) {
      const Status failure = Status(
          StatusCode::ReadNotCurrent,
          "the nominated source replica is not current and cannot be a source of truth",
          std::string(to_token(it->second.state)));
      outcome.reason = failure.code();
      outcome.rationale = failure.to_string();
      return Result<SyncOutcome>::failure(failure);
    }
  } else {
    for (const RegionId id : object.replicas) {
      if (id == destination->id) continue;
      const auto it = impl_->regions.find(id);
      if (it == impl_->regions.end()) continue;
      if (it->second.state != CoherenceState::Current) continue;
      if (object.authoritative_version.defined() &&
          it->second.version != object.authoritative_version) {
        continue;
      }
      source_id = id;
      break;
    }
  }
  if (!source_id.defined()) {
    const Status failure =
        Status(StatusCode::NotAuthoritative,
               "no replica currently holds the authoritative version, so there is nothing to "
               "synchronize from");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  const RegionRecord& source = impl_->regions.at(source_id);

  impl_->tick();
  SyncRecord record;
  record.id = SyncOperationId::from_value(impl_->next_sync_id++);
  record.kind = request.kind;
  record.object = object.id;
  record.object_generation = object.generation;
  record.source_region = source.id;
  record.source_region_generation = source.generation;
  record.destination_region = destination->id;
  record.destination_region_generation = destination->generation;
  record.source_version = object.authoritative_version.defined() ? object.authoritative_version
                                                                : source.version;
  record.destination_prior_version = destination->version;
  record.extent_offset = source.offset;
  record.extent_length = source.length;
  record.expected_content = source.content;
  record.ownership_generation = object.ownership_generation;
  record.policy = object.policy;
  record.policy_generation = object.policy_generation;
  record.epoch = impl_->epoch;
  record.destination_participant = destination->participant;
  record.destination_boot = destination->boot;
  record.state = SyncState::Requested;
  record.transport = request.transport.empty() ? std::string("delegated") : request.transport;
  record.postcondition = "destination replica holds version " + record.source_version.to_string() +
                         " with fingerprint " + record.expected_content.to_string() +
                         " and becomes current under ownership generation " +
                         object.ownership_generation.to_string();
  record.issued_sequence = impl_->sequence;

  const CoherenceState previous = destination->state;
  if (previous == CoherenceState::RevalidationRequired) {
    // Revalidation keeps its own state; the synchronization makes it current on
    // completion, but until then it is still not current.
  } else if (previous != CoherenceState::Retired && previous != CoherenceState::Fenced) {
    destination->state = CoherenceState::SyncRequired;
  }
  destination->note = "synchronization " + record.id.to_string() + " requested from " +
                      source.id.to_string();
  destination->updated_sequence = impl_->sequence;

  std::vector<JournalEntry> entries;
  push_sync_entry(entries, record);
  push_region_entry(entries, *destination);
  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) return Result<SyncOutcome>::failure(written);

  impl_->syncs[record.id] = record;
  outcome.operation = record.id;
  outcome.state = record.state;
  outcome.plan = record;
  outcome.reason = StatusCode::Ok;
  outcome.rationale = "synchronization plan issued; a plan is not a completion";
  impl_->record_decision(DecisionKind::SyncBegin, outcome.context, StatusCode::Ok,
                         outcome.rationale);
  return Result<SyncOutcome>::success(outcome);
}

Result<SyncOutcome> CoherenceEngine::complete_sync(const SyncCompleteRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  SyncOutcome outcome;
  const auto sync_it = impl_->syncs.find(request.operation);
  if (sync_it == impl_->syncs.end()) {
    const Status failure = Status(StatusCode::UnknownSyncOperation,
                                  "no such synchronization operation",
                                  request.operation.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  SyncRecord& record = sync_it->second;
  Resolved resolved;
  Status authority = resolve_locked(*impl_, request.context, resolved);
  outcome.context = impl_->make_context(request.context, record.object, record.object_generation,
                                        record.ownership_generation);
  outcome.operation = record.id;
  outcome.plan = record;
  if (!authority.ok()) {
    outcome.reason = authority.code();
    outcome.rationale = authority.to_string();
    return Result<SyncOutcome>::failure(authority);
  }
  if (resolved.object->id != record.object) {
    const Status failure =
        Status(StatusCode::InvalidArgument, "the completion names a synchronization for another object");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  if (record.state == SyncState::Completed) {
    // Duplicate completion: report the original outcome without re-applying it.
    outcome.state = record.state;
    outcome.reason = StatusCode::Ok;
    outcome.idempotent_replay = true;
    outcome.rationale = "the synchronization was already completed; no second effect is applied";
    return Result<SyncOutcome>::success(outcome);
  }
  if (record.state == SyncState::Cancelled || record.state == SyncState::Failed) {
    const Status failure = Status(StatusCode::InvalidTransition,
                                  "the synchronization is already settled",
                                  std::string(to_token(record.state)));
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  if (record.destination_participant != request.context.participant) {
    const Status failure = Status(StatusCode::NotWriteAuthorized,
                                  "the completion does not come from the destination participant");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  if (record.destination_boot != request.context.boot) {
    const Status failure = Status(
        StatusCode::StaleBoot,
        "the completion carries a boot identity that is not the boot the plan was issued to",
        "plan_boot=" + record.destination_boot.to_string() +
            " caller_boot=" + request.context.boot.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  if (request.destination_region != record.destination_region ||
      request.destination_region_generation != record.destination_region_generation) {
    const Status failure = Status(
        StatusCode::StaleRegionGeneration,
        "the completion targets a different region generation than the plan was issued for",
        "plan_region=" + record.destination_region.to_string() +
            " plan_generation=" + record.destination_region_generation.to_string() +
            " caller_region=" + request.destination_region.to_string() +
            " caller_generation=" + request.destination_region_generation.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  if (request.destination_new_version != record.source_version) {
    const Status failure =
        Status(StatusCode::StaleSyncOperation,
               "the completion names a different version than the plan required",
               "plan_version=" + record.source_version.to_string() +
                   " reported_version=" + request.destination_new_version.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }

  const auto region_it = impl_->regions.find(record.destination_region);
  if (region_it == impl_->regions.end()) {
    const Status failure = Status(StatusCode::UnknownRegion,
                                  "the destination replica no longer exists");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  RegionRecord& destination = region_it->second;

  // Retirement revokes live authority permanently. A transfer that completes
  // afterwards must never bring a retired replica back into service.
  if (destination.lifecycle == RegionLifecycle::Retired ||
      destination.state == CoherenceState::Retired) {
    impl_->tick();
    record.state = SyncState::Cancelled;
    record.settled_sequence = impl_->sequence;
    record.failure_reason =
        "the destination replica was retired before the transfer completed";
    std::vector<JournalEntry> entries;
    push_sync_entry(entries, record);
    Status written = impl_->append_many_locked(entries);
    if (!written.ok()) return Result<SyncOutcome>::failure(written);
    outcome.state = record.state;
    outcome.plan = record;
    outcome.reason = StatusCode::Retired;
    outcome.rationale = record.failure_reason;
    impl_->record_decision(DecisionKind::SyncComplete, outcome.context, outcome.reason,
                           outcome.rationale);
    return Result<SyncOutcome>::success(outcome);
  }

  // The authoritative version must not have advanced underneath the transfer.
  // A stale completion must never certify currentness at a superseded version.
  if (resolved.object->authoritative_version != record.source_version) {
    impl_->tick();
    record.state = SyncState::Failed;
    record.settled_sequence = impl_->sequence;
    record.failure_reason =
        "the authoritative version advanced while the transfer was in flight";
    destination.state = CoherenceState::Stale;
    destination.note = "synchronization completed against a superseded version " +
                       record.source_version.to_string();
    destination.updated_sequence = impl_->sequence;
    std::vector<JournalEntry> entries;
    push_sync_entry(entries, record);
    push_region_entry(entries, destination);
    Status written = impl_->append_many_locked(entries);
    if (!written.ok()) return Result<SyncOutcome>::failure(written);
    outcome.state = record.state;
    outcome.reason = StatusCode::StalePublication;
    outcome.rationale = record.failure_reason;
    impl_->record_decision(DecisionKind::SyncComplete, outcome.context, outcome.reason,
                           outcome.rationale);
    return Result<SyncOutcome>::success(outcome);
  }

  const CoherencePolicy* policy = policy_locked(*impl_, *resolved.object);
  const bool verify = policy != nullptr && policy->require_content_fingerprint;
  if (verify && record.expected_content.defined) {
    if (!request.content_moved) {
      impl_->tick();
      record.state = SyncState::Failed;
      record.settled_sequence = impl_->sequence;
      record.failure_reason = "the caller reported that no bytes were moved";
      destination.state = CoherenceState::SyncRequired;
      destination.note = record.failure_reason;
      destination.updated_sequence = impl_->sequence;
      std::vector<JournalEntry> entries;
      push_sync_entry(entries, record);
      push_region_entry(entries, destination);
      (void)impl_->append_many_locked(entries);
      outcome.state = record.state;
      outcome.reason = StatusCode::ContentMismatch;
      outcome.rationale = record.failure_reason;
      return Result<SyncOutcome>::success(outcome);
    }
    if (!(request.observed_content == record.expected_content)) {
      impl_->tick();
      record.state = SyncState::Failed;
      record.settled_sequence = impl_->sequence;
      record.failure_reason =
          "the destination fingerprint does not match the published content fingerprint: expected " +
          record.expected_content.to_string() + " observed " + request.observed_content.to_string();
      destination.state = CoherenceState::RevalidationRequired;
      destination.note = record.failure_reason;
      destination.updated_sequence = impl_->sequence;
      std::vector<JournalEntry> entries;
      push_sync_entry(entries, record);
      push_region_entry(entries, destination);
      Status written = impl_->append_many_locked(entries);
      if (!written.ok()) return Result<SyncOutcome>::failure(written);
      outcome.state = record.state;
      outcome.reason = StatusCode::ContentMismatch;
      outcome.rationale = record.failure_reason;
      impl_->record_decision(DecisionKind::SyncComplete, outcome.context, outcome.reason,
                             outcome.rationale);
      return Result<SyncOutcome>::success(outcome);
    }
  }

  impl_->tick();
  record.state = SyncState::Completed;
  record.settled_sequence = impl_->sequence;
  destination.state = CoherenceState::Current;
  destination.version = record.source_version;
  destination.last_synced_version = record.source_version;
  destination.ownership_generation = resolved.object->ownership_generation;
  destination.dirty = DirtyCondition::Clean;
  destination.dirty_published = false;
  if (request.observed_content.defined) destination.content = request.observed_content;

  const EvidenceRecord evidence = impl_->make_evidence(
      request.content_moved && request.observed_content.defined ? EvidenceKind::ByteComparison
                                                               : EvidenceKind::BackendCompletion,
      destination.evidence_class, request.context.participant, request.context.boot,
      resolved.object->id, resolved.object->generation, destination.id, destination.generation,
      record.source_version, destination.content);
  destination.evidence = evidence.id;
  destination.evidence_generation = evidence.generation;
  destination.note = "synchronized to version " + record.source_version.to_string() +
                     " from region " + record.source_region.to_string();
  destination.updated_sequence = impl_->sequence;

  std::vector<JournalEntry> entries;
  push_sync_entry(entries, record);
  push_region_entry(entries, destination);
  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) return Result<SyncOutcome>::failure(written);

  outcome.state = record.state;
  outcome.plan = record;
  outcome.reason = StatusCode::Ok;
  outcome.rationale =
      "synchronization completed: the destination replica is current at the published version and "
      "carries evidence produced by an actual byte comparison";
  impl_->record_decision(DecisionKind::SyncComplete, outcome.context, StatusCode::Ok,
                         outcome.rationale);
  return Result<SyncOutcome>::success(outcome);
}

Result<SyncOutcome> CoherenceEngine::fail_sync(const SyncFailRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  SyncOutcome outcome;
  const auto sync_it = impl_->syncs.find(request.operation);
  if (sync_it == impl_->syncs.end()) {
    const Status failure = Status(StatusCode::UnknownSyncOperation,
                                  "no such synchronization operation",
                                  request.operation.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  SyncRecord& record = sync_it->second;
  outcome.operation = record.id;
  outcome.context = impl_->make_context(request.context, record.object, record.object_generation,
                                        record.ownership_generation);
  if (record.state == SyncState::Completed) {
    outcome.state = record.state;
    outcome.plan = record;
    outcome.reason = StatusCode::InvalidTransition;
    outcome.rationale =
        "the synchronization already completed; a failure report cannot undo it";
    return Result<SyncOutcome>::success(outcome);
  }
  if (record.destination_participant != request.context.participant ||
      record.destination_boot != request.context.boot) {
    const Status failure =
        Status(StatusCode::NotWriteAuthorized,
               "the failure report does not come from the destination participant incarnation");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<SyncOutcome>::failure(failure);
  }
  impl_->tick();
  record.state = request.outcome_unknown ? SyncState::OutcomeUnknown : SyncState::Failed;
  record.settled_sequence = impl_->sequence;
  record.failure_reason = request.detail.empty()
                              ? std::string(status_code_name(request.cause))
                              : request.detail;

  const auto region_it = impl_->regions.find(record.destination_region);
  if (region_it != impl_->regions.end()) {
    RegionRecord& destination = region_it->second;
    if (request.outcome_unknown) {
      // The outcome is genuinely unknown. The replica must not be treated as
      // current, and the runtime does not guess whether bytes arrived.
      destination.state = CoherenceState::SyncRequired;
      destination.note = "synchronization outcome unknown: " + record.failure_reason;
    } else if (request.cause == StatusCode::ContentMismatch) {
      destination.state = CoherenceState::RevalidationRequired;
      destination.note = "content mismatch during synchronization: " + record.failure_reason;
    } else {
      destination.state = CoherenceState::Invalid;
      destination.note = "synchronization failed: " + record.failure_reason;
    }
    destination.updated_sequence = impl_->sequence;
    std::vector<JournalEntry> entries;
    push_sync_entry(entries, record);
    push_region_entry(entries, destination);
    Status written = impl_->append_many_locked(entries);
    if (!written.ok()) return Result<SyncOutcome>::failure(written);
  } else {
    std::vector<JournalEntry> entries;
    push_sync_entry(entries, record);
    Status written = impl_->append_many_locked(entries);
    if (!written.ok()) return Result<SyncOutcome>::failure(written);
  }

  outcome.state = record.state;
  outcome.plan = record;
  outcome.reason = StatusCode::Ok;
  outcome.rationale = request.outcome_unknown
                          ? "synchronization outcome recorded as unknown; the replica is not "
                            "current and will require re-synchronization"
                          : "synchronization failure recorded";
  impl_->record_decision(DecisionKind::SyncComplete, outcome.context, StatusCode::Ok,
                         outcome.rationale);
  return Result<SyncOutcome>::success(outcome);
}

} // namespace coherence
