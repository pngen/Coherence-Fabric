// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Dirty state, publication, release, invalidation and synchronization.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "coherence/engine.hpp"
#include "engine_helpers.hpp"
#include "engine_impl.hpp"

namespace coherence {

using engine_detail::policy_locked;
using engine_detail::push_invalidation_entry;
using engine_detail::push_object_entry;
using engine_detail::push_region_entry;
using engine_detail::Resolved;
using engine_detail::resolve_internal_locked;
using engine_detail::resolve_locked;
using engine_detail::resolve_region_locked;

// ---------------------------------------------------------------------------
// Dirty state
// ---------------------------------------------------------------------------
Result<DirtyCondition> CoherenceEngine::mark_dirty(const DirtyRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Resolved resolved;
  Status authority = resolve_locked(*impl_, request.context, resolved);
  const DecisionContext context = impl_->make_context(
      request.context, request.context.object, request.context.object_generation,
      resolved.object != nullptr ? resolved.object->ownership_generation
                                 : OwnershipGeneration::nil());
  if (!authority.ok()) {
    impl_->record_decision(DecisionKind::MarkDirty, context, authority.code(), authority.to_string());
    return Result<DirtyCondition>::failure(authority);
  }
  ObjectRecord& object = *resolved.object;
  if (object.authority != AuthorityMode::ExclusiveWriter) {
    const Status failure =
        object.authority == AuthorityMode::TransferPending
            ? Status(StatusCode::InvalidationOutstanding,
                     "mandatory invalidations are still outstanding for this write transfer")
            : Status(StatusCode::NotWriteAuthorized,
                     "the caller does not hold exclusive write authority");
    impl_->record_decision(DecisionKind::MarkDirty, context, failure.code(), failure.to_string());
    return Result<DirtyCondition>::failure(failure);
  }
  if (object.writer != request.context.participant || object.writer_boot != request.context.boot) {
    const Status failure = Status(StatusCode::NotWriteAuthorized,
                                  "the caller is not the holder of write authority for this object",
                                  "writer=" + object.writer.to_string());
    impl_->record_decision(DecisionKind::MarkDirty, context, failure.code(), failure.to_string());
    return Result<DirtyCondition>::failure(failure);
  }
  if (request.ownership_generation != object.ownership_generation) {
    const Status failure =
        Status(StatusCode::StaleOwnership, "request carries a stale ownership generation",
               "request=" + request.ownership_generation.to_string() +
                   " current=" + object.ownership_generation.to_string());
    impl_->record_decision(DecisionKind::MarkDirty, context, failure.code(), failure.to_string());
    return Result<DirtyCondition>::failure(failure);
  }
  if (request.base_version != object.authoritative_version) {
    const Status failure =
        Status(StatusCode::StalePublication, "mark-dirty carries a stale base version",
               "request=" + request.base_version.to_string() +
                   " current=" + object.authoritative_version.to_string());
    impl_->record_decision(DecisionKind::MarkDirty, context, failure.code(), failure.to_string());
    return Result<DirtyCondition>::failure(failure);
  }
  RegionRecord* region = nullptr;
  Status resolved_region = resolve_region_locked(*impl_, resolved, request.region,
                                                 request.region_generation, region);
  if (!resolved_region.ok()) {
    impl_->record_decision(DecisionKind::MarkDirty, context, resolved_region.code(),
                           resolved_region.to_string());
    return Result<DirtyCondition>::failure(resolved_region);
  }
  if (region->participant != request.context.participant) {
    const Status failure = Status(StatusCode::NotWriteAuthorized,
                                  "the caller does not own the region it is marking dirty");
    impl_->record_decision(DecisionKind::MarkDirty, context, failure.code(), failure.to_string());
    return Result<DirtyCondition>::failure(failure);
  }
  if (region->state != CoherenceState::Current && region->state != CoherenceState::Dirty) {
    const Status failure =
        Status(StatusCode::SyncRequired,
               "a replica that is not current cannot become the source of a new version",
               std::string(to_token(region->state)));
    impl_->record_decision(DecisionKind::MarkDirty, context, failure.code(), failure.to_string());
    return Result<DirtyCondition>::failure(failure);
  }
  if (!is_legal_coherence_transition(region->state, CoherenceState::Dirty)) {
    const Status failure = Status(StatusCode::InvalidTransition,
                                  "illegal coherence transition to dirty",
                                  std::string(to_token(region->state)));
    impl_->record_decision(DecisionKind::MarkDirty, context, failure.code(), failure.to_string());
    return Result<DirtyCondition>::failure(failure);
  }

  impl_->tick();
  region->dirty = DirtyCondition::DirtyUnpublished;
  region->dirty_base_version = request.base_version;
  region->dirty_published = false;
  region->state = CoherenceState::Dirty;
  region->ownership_generation = object.ownership_generation;
  region->updated_sequence = impl_->sequence;
  region->note = "unpublished modification based on version " + request.base_version.to_string();
  if (request.content.defined) region->content = request.content;
  object.has_unpublished_dirty = true;
  object.dirty_condition = DirtyCondition::DirtyUnpublished;
  object.updated_sequence = impl_->sequence;

  std::vector<JournalEntry> entries;
  push_object_entry(entries, object);
  push_region_entry(entries, *region);
  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) return Result<DirtyCondition>::failure(written);

  impl_->record_decision(DecisionKind::MarkDirty, context, StatusCode::Ok,
                         "replica marked dirty against base version " +
                             request.base_version.to_string());
  return Result<DirtyCondition>::success(DirtyCondition::DirtyUnpublished);
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------
Result<InvalidationOutcome> CoherenceEngine::invalidate(const InvalidationRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  ObjectRecord* object = nullptr;
  Status authority = resolve_internal_locked(*impl_, request.context, object);
  InvalidationOutcome outcome;
  if (object != nullptr) {
    outcome.context = impl_->make_context(request.context, request.context.object,
                                          request.context.object_generation,
                                          object->ownership_generation);
  }
  if (!authority.ok()) {
    outcome.reason = authority.code();
    outcome.rationale = authority.to_string();
    return Result<InvalidationOutcome>::failure(authority);
  }
  if (request.ownership_generation != object->ownership_generation) {
    const Status failure =
        Status(StatusCode::StaleOwnership, "invalidation carries a stale ownership generation",
               "request=" + request.ownership_generation.to_string() +
                   " current=" + object->ownership_generation.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<InvalidationOutcome>::failure(failure);
  }
  const auto region_it = impl_->regions.find(request.target_region);
  if (region_it == impl_->regions.end()) {
    const Status failure = Status(StatusCode::UnknownRegion, "no such region",
                                  request.target_region.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<InvalidationOutcome>::failure(failure);
  }
  RegionRecord& region = region_it->second;
  if (region.object != object->id) {
    const Status failure = Status(StatusCode::InvalidArgument,
                                  "the invalidation target does not belong to the object");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<InvalidationOutcome>::failure(failure);
  }
  // Generation binding: an invalidation of generation N must never touch a
  // replacement replica at N+1.
  if (region.generation != request.target_region_generation ||
      region.replica_generation != request.target_replica_generation) {
    const Status failure = Status(
        StatusCode::StaleRegionGeneration,
        "the invalidation target generation no longer matches the live replica",
        "request_region_generation=" + request.target_region_generation.to_string() +
            " live_region_generation=" + region.generation.to_string() +
            " request_replica_generation=" + request.target_replica_generation.to_string() +
            " live_replica_generation=" + region.replica_generation.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<InvalidationOutcome>::failure(failure);
  }

  // An inert replica is already not a source of truth. The invalidation is
  // satisfied vacuously and is recorded as superseded rather than left
  // outstanding forever, and it is never carried over to a replacement.
  if (region.lifecycle == RegionLifecycle::Retired || region.state == CoherenceState::Retired ||
      region.state == CoherenceState::Fenced) {
    impl_->tick();
    InvalidationRecord record;
    record.id = InvalidationId::from_value(impl_->next_invalidation_id++);
    record.object = object->id;
    record.object_generation = object->generation;
    record.region = region.id;
    record.region_generation = region.generation;
    record.replica_generation = region.replica_generation;
    record.published_version = request.published_version.defined()
                                   ? request.published_version
                                   : object->authoritative_version;
    record.superseded_version = region.version;
    record.ownership_generation = object->ownership_generation;
    record.epoch = impl_->epoch;
    record.target_participant = region.participant;
    record.target_boot = region.boot;
    record.acknowledgement_required = false;
    record.issued_sequence = impl_->sequence;
    record.settled_sequence = impl_->sequence;
    record.state = InvalidationState::Superseded;
    record.rationale =
        "the targeted replica is inert (" + std::string(to_token(region.state)) +
        "); the invalidation is satisfied vacuously and is not applied to any replacement";
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertInvalidation;
    entry.invalidation = record;
    Status written = impl_->append_locked(entry);
    if (!written.ok()) return Result<InvalidationOutcome>::failure(written);
    impl_->invalidations[record.id] = record;
    outcome.invalidation = record.id;
    outcome.state = record.state;
    outcome.reason = StatusCode::Ok;
    outcome.region = region.id;
    outcome.region_generation = region.generation;
    outcome.replica_generation = region.replica_generation;
    outcome.idempotent_replay = true;
    outcome.rationale = record.rationale;
    impl_->record_decision(DecisionKind::Invalidate, outcome.context, StatusCode::Ok,
                           outcome.rationale);
    return Result<InvalidationOutcome>::success(outcome);
  }

  // Idempotency: an already acknowledged invalidation for the same target is a
  // successful replay, not a second effect.
  for (const InvalidationId id : object->outstanding_invalidations) {
    const auto it = impl_->invalidations.find(id);
    if (it == impl_->invalidations.end()) continue;
    if (it->second.region == region.id && it->second.region_generation == region.generation &&
        it->second.replica_generation == region.replica_generation) {
      outcome.invalidation = id;
      outcome.state = it->second.state;
      outcome.reason = StatusCode::Ok;
      outcome.idempotent_replay = true;
      outcome.region = region.id;
      outcome.region_generation = region.generation;
      outcome.replica_generation = region.replica_generation;
      outcome.rationale = "an identical invalidation is already outstanding or acknowledged";
      return Result<InvalidationOutcome>::success(outcome);
    }
  }

  impl_->tick();
  InvalidationRecord record;
  record.id = InvalidationId::from_value(impl_->next_invalidation_id++);
  record.object = object->id;
  record.object_generation = object->generation;
  record.region = region.id;
  record.region_generation = region.generation;
  record.replica_generation = region.replica_generation;
  record.published_version = request.published_version.defined() ? request.published_version
                                                                 : object->authoritative_version;
  record.superseded_version = region.version;
  record.ownership_generation = object->ownership_generation;
  record.epoch = impl_->epoch;
  record.target_participant = region.participant;
  record.target_boot = region.boot;
  record.acknowledgement_required = request.acknowledgement_required;
  record.issued_sequence = impl_->sequence;
  record.rationale = "explicit generation-bound invalidation";

  object->outstanding_invalidations.push_back(record.id);
  std::sort(object->outstanding_invalidations.begin(), object->outstanding_invalidations.end());
  object->updated_sequence = impl_->sequence;

  std::vector<JournalEntry> entries;
  JournalEntry invalidation_entry;
  invalidation_entry.kind = JournalEntryKind::UpsertInvalidation;
  invalidation_entry.invalidation = record;
  entries.push_back(std::move(invalidation_entry));

  if (!request.acknowledgement_required) {
    // No acknowledgement required: the effect is applied immediately and the
    // record is settled in the same transaction.
    record.state = InvalidationState::Acknowledged;
    record.settled_sequence = impl_->sequence;
    region.state = region.version.defined() ? CoherenceState::Stale : CoherenceState::Invalid;
    region.evidence = EvidenceId::nil();
    region.evidence_generation = EvidenceGeneration::nil();
    region.invalidation_pending = false;
    region.pending_invalidation = InvalidationId::nil();
    region.pending_invalidation_target = RegionGeneration::nil();
    region.updated_sequence = impl_->sequence;
    region.note = "invalidated by " + record.id.to_string();
    object->outstanding_invalidations.erase(
        std::remove(object->outstanding_invalidations.begin(),
                    object->outstanding_invalidations.end(), record.id),
        object->outstanding_invalidations.end());
    impl_->recompute_authority_locked(*object);
    JournalEntry settled;
    settled.kind = JournalEntryKind::UpsertInvalidation;
    settled.invalidation = record;
    entries.push_back(std::move(settled));
    push_region_entry(entries, region);
    push_object_entry(entries, *object);
  } else {
    region.invalidation_pending = true;
    region.pending_invalidation = record.id;
    region.pending_invalidation_target = region.generation;
    region.updated_sequence = impl_->sequence;
    push_region_entry(entries, region);
    push_object_entry(entries, *object);
  }

  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) return Result<InvalidationOutcome>::failure(written);

  impl_->invalidations[record.id] = record;
  outcome.invalidation = record.id;
  outcome.state = record.state;
  outcome.reason = StatusCode::Ok;
  outcome.region = region.id;
  outcome.region_generation = region.generation;
  outcome.replica_generation = region.replica_generation;
  outcome.rationale = request.acknowledgement_required
                          ? "invalidation issued and awaiting acknowledgement"
                          : "invalidation applied and settled without acknowledgement";
  impl_->record_decision(DecisionKind::Invalidate, outcome.context, StatusCode::Ok,
                         outcome.rationale);
  return Result<InvalidationOutcome>::success(outcome);
}

Result<InvalidationOutcome> CoherenceEngine::acknowledge_invalidation(
    const InvalidationAck& acknowledgement) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  InvalidationOutcome outcome;
  const auto invalidation_it = impl_->invalidations.find(acknowledgement.invalidation);
  if (invalidation_it == impl_->invalidations.end()) {
    const Status failure = Status(StatusCode::UnknownInvalidation,
                                  "no such invalidation", acknowledgement.invalidation.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<InvalidationOutcome>::failure(failure);
  }
  InvalidationRecord& record = invalidation_it->second;
  Resolved resolved;
  Status authority = resolve_locked(*impl_, acknowledgement.context, resolved);
  outcome.context = impl_->make_context(acknowledgement.context, record.object,
                                        record.object_generation, record.ownership_generation);
  outcome.invalidation = record.id;
  outcome.region = record.region;
  outcome.region_generation = record.region_generation;
  outcome.replica_generation = record.replica_generation;
  if (!authority.ok()) {
    outcome.reason = authority.code();
    outcome.rationale = authority.to_string();
    return Result<InvalidationOutcome>::failure(authority);
  }
  ObjectRecord& object = *resolved.object;
  if (resolved.object->id != record.object) {
    const Status failure = Status(StatusCode::InvalidArgument,
                                  "the acknowledgement names an invalidation for a different object");
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<InvalidationOutcome>::failure(failure);
  }
  // Only the targeted participant incarnation may acknowledge.
  if (record.target_participant != acknowledgement.context.participant ||
      record.target_boot != acknowledgement.context.boot) {
    const Status failure =
        Status(StatusCode::NotWriteAuthorized,
               "the acknowledgement does not come from the participant the invalidation targeted",
               "target=" + record.target_participant.to_string() +
                   " caller=" + acknowledgement.context.participant.to_string());
    outcome.reason = failure.code();
    outcome.rationale = failure.to_string();
    return Result<InvalidationOutcome>::failure(failure);
  }
  if (record.state == InvalidationState::Acknowledged) {
    outcome.state = record.state;
    outcome.reason = StatusCode::Ok;
    outcome.idempotent_replay = true;
    outcome.rationale = "the invalidation was already acknowledged; no second effect is applied";
    return Result<InvalidationOutcome>::success(outcome);
  }
  if (record.state == InvalidationState::Superseded) {
    outcome.state = record.state;
    outcome.reason = StatusCode::StaleInvalidation;
    outcome.rationale = "the invalidation was superseded; the acknowledgement has no effect";
    return Result<InvalidationOutcome>::success(outcome);
  }

  const auto region_it = impl_->regions.find(record.region);
  if (region_it == impl_->regions.end()) {
    // The replica disappeared. The invalidation is satisfied vacuously and is
    // never applied to a later incarnation.
    record.state = InvalidationState::Superseded;
    record.settled_sequence = impl_->sequence;
    record.rationale = "target replica no longer exists";
    object.outstanding_invalidations.erase(
        std::remove(object.outstanding_invalidations.begin(),
                    object.outstanding_invalidations.end(), record.id),
        object.outstanding_invalidations.end());
    impl_->recompute_authority_locked(object);
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertInvalidation;
    entry.invalidation = record;
    Status written = impl_->append_locked(entry);
    if (!written.ok()) return Result<InvalidationOutcome>::failure(written);
    outcome.state = record.state;
    outcome.reason = StatusCode::Ok;
    outcome.rationale = record.rationale;
    return Result<InvalidationOutcome>::success(outcome);
  }
  RegionRecord& region = region_it->second;
  if (region.generation != record.region_generation ||
      region.replica_generation != record.replica_generation ||
      region.generation != acknowledgement.region_generation ||
      region.replica_generation != acknowledgement.replica_generation) {
    // The replica was replaced. The acknowledgement belongs to a generation
    // that no longer exists and must not affect the replacement.
    record.state = InvalidationState::Superseded;
    record.settled_sequence = impl_->sequence;
    record.rationale =
        "the targeted replica generation was replaced before the acknowledgement arrived";
    object.outstanding_invalidations.erase(
        std::remove(object.outstanding_invalidations.begin(),
                    object.outstanding_invalidations.end(), record.id),
        object.outstanding_invalidations.end());
    impl_->recompute_authority_locked(object);
    std::vector<JournalEntry> entries;
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertInvalidation;
    entry.invalidation = record;
    entries.push_back(std::move(entry));
    push_object_entry(entries, object);
    Status written = impl_->append_many_locked(entries);
    if (!written.ok()) return Result<InvalidationOutcome>::failure(written);
    outcome.state = record.state;
    outcome.reason = StatusCode::Ok;
    outcome.rationale = record.rationale;
    return Result<InvalidationOutcome>::success(outcome);
  }

  impl_->tick();
  record.state = InvalidationState::Acknowledged;
  record.settled_sequence = impl_->sequence;
  record.superseded_version = acknowledgement.superseded_version.defined()
                                  ? acknowledgement.superseded_version
                                  : region.version;
  // The invalidated replica may hold bytes, but those bytes are no longer a
  // source of truth and may not satisfy a current read.
  region.state = region.version.defined() ? CoherenceState::Stale : CoherenceState::Invalid;
  region.evidence = EvidenceId::nil();
  region.evidence_generation = EvidenceGeneration::nil();
  region.invalidation_pending = false;
  region.pending_invalidation = InvalidationId::nil();
  region.pending_invalidation_target = RegionGeneration::nil();
  region.updated_sequence = impl_->sequence;
  region.note = "invalidated by " + record.id.to_string();
  object.outstanding_invalidations.erase(
      std::remove(object.outstanding_invalidations.begin(), object.outstanding_invalidations.end(),
                  record.id),
      object.outstanding_invalidations.end());
  impl_->recompute_authority_locked(object);
  object.updated_sequence = impl_->sequence;

  std::vector<JournalEntry> entries;
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertInvalidation;
  entry.invalidation = record;
  entries.push_back(std::move(entry));
  push_region_entry(entries, region);
  push_object_entry(entries, object);
  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) return Result<InvalidationOutcome>::failure(written);

  outcome.state = record.state;
  outcome.reason = StatusCode::Ok;
  outcome.rationale = "invalidation acknowledged and applied to the targeted replica generation";
  impl_->record_decision(DecisionKind::Invalidate, outcome.context, StatusCode::Ok,
                         outcome.rationale);
  return Result<InvalidationOutcome>::success(outcome);
}

// ---------------------------------------------------------------------------
// Revalidation
// ---------------------------------------------------------------------------
Result<RegionRecord> CoherenceEngine::revalidate_region(const RevalidateRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Resolved resolved;
  Status authority = resolve_locked(*impl_, request.context, resolved);
  if (!authority.ok()) return Result<RegionRecord>::failure(authority);
  RegionRecord* region = nullptr;
  Status resolved_region = resolve_region_locked(*impl_, resolved, request.region,
                                                 request.region_generation, region);
  if (!resolved_region.ok()) return Result<RegionRecord>::failure(resolved_region);
  if (region->participant != request.context.participant || region->boot != request.context.boot) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::NotWriteAuthorized,
        "only the owning participant incarnation may revalidate a replica"));
  }
  if (region->lifecycle == RegionLifecycle::Retired) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::Retired, "a retired replica cannot be revalidated"));
  }
  const CoherencePolicy* policy = policy_locked(*impl_, *resolved.object);
  if (policy == nullptr) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::UnknownPolicy, "the object references a policy that is not present"));
  }
  if (policy->require_content_fingerprint && !request.content.defined) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::EvidenceMissing,
        "the active policy requires a content fingerprint to revalidate a replica"));
  }
  if (!request.byte_compared && policy->require_content_fingerprint) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::EvidenceMissing,
        "an assertion without a byte comparison cannot revalidate a replica under this policy"));
  }
  if (!request.observed_version.defined()) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::InvalidArgument, "revalidation must name the version actually observed"));
  }

  impl_->tick();
  const VersionId authoritative = resolved.object->authoritative_version;
  const bool matches_authoritative = authoritative.defined() &&
                                     request.observed_version == authoritative;
  const bool establishes_initial = !authoritative.defined();

  const EvidenceRecord evidence = impl_->make_evidence(
      request.byte_compared ? EvidenceKind::ByteComparison : EvidenceKind::RuntimeObservation,
      region->evidence_class, request.context.participant, resolved.participant->boot,
      resolved.object->id, resolved.object->generation, region->id, region->generation,
      request.observed_version, request.content);

  region->version = request.observed_version;
  region->ownership_generation = resolved.object->ownership_generation;
  region->evidence = evidence.id;
  region->evidence_generation = evidence.generation;
  region->updated_sequence = impl_->sequence;
  if (request.content.defined) region->content = request.content;
  if (region->dirty == DirtyCondition::DirtyUnpublished) {
    region->dirty = DirtyCondition::DirtyPublished;
    region->dirty_published = true;
  }
  if (matches_authoritative || establishes_initial) {
    region->state = CoherenceState::Current;
    region->last_synced_version = request.observed_version;
    region->note = "revalidated by direct observation at version " +
                   request.observed_version.to_string();
  } else {
    region->state = CoherenceState::Stale;
    region->note = "revalidation observed version " + request.observed_version.to_string() +
                   " which is not the authoritative version " + authoritative.to_string();
  }

  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertRegion;
  entry.region = *region;
  Status written = impl_->append_locked(entry);
  if (!written.ok()) return Result<RegionRecord>::failure(written);

  const RegionRecord copy = *region;
  const DecisionContext context = impl_->make_context(
      request.context, resolved.object->id, resolved.object->generation,
      resolved.object->ownership_generation);
  impl_->record_decision(DecisionKind::Revalidate, context, StatusCode::Ok,
                         copy.note);
  return Result<RegionRecord>::success(copy);
}

} // namespace coherence
