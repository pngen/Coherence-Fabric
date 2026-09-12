// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Coherence operations: read authority and write authority.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "coherence/engine.hpp"
#include "engine_helpers.hpp"
#include "engine_impl.hpp"

namespace coherence {

using engine_detail::add_read_grant_locked;
using engine_detail::assess_region_locked;
using engine_detail::policy_locked;
using engine_detail::Resolved;
using engine_detail::resolve_locked;
using engine_detail::resolve_region_locked;
using engine_detail::select_read_region_locked;

namespace {

Result<ReadDecision> compute_read_locked(CoherenceEngine::Impl& state, const ReadRequest& request,
                                         bool create_lease) {
  Resolved resolved;
  Status authority = resolve_locked(state, request.context, resolved);
  DecisionContext context = state.make_context(request.context, request.context.object,
                                               request.context.object_generation,
                                               resolved.object != nullptr
                                                   ? resolved.object->ownership_generation
                                                   : OwnershipGeneration::nil());
  ReadDecision decision;
  decision.context = context;
  decision.region_generation = RegionGeneration::nil();

  if (!authority.ok()) {
    decision.outcome = authority.code() == StatusCode::UnknownObject ? ReadOutcome::Unknown
                                                                    : ReadOutcome::ReadBlocked;
    decision.reason = authority.code();
    decision.rationale = authority.to_string();
    decision.region_state = CoherenceState::Unknown;
    state.record_decision(DecisionKind::ReadAcquire, context, authority.code(), decision.rationale);
    return Result<ReadDecision>::success(decision);
  }
  const CoherencePolicy* policy = policy_locked(state, *resolved.object);
  if (policy == nullptr) {
    decision.outcome = ReadOutcome::Unknown;
    decision.reason = StatusCode::UnknownPolicy;
    decision.rationale = "the object references a policy that is not present";
    state.record_decision(DecisionKind::ReadAcquire, context, StatusCode::UnknownPolicy,
                          decision.rationale);
    return Result<ReadDecision>::failure(Status(StatusCode::UnknownPolicy, decision.rationale));
  }
  if (policy->write_ownership == WriteOwnershipMode::Unspecified ||
      policy->consistency == ConsistencyModel::Unspecified) {
    decision.outcome = ReadOutcome::Unsupported;
    decision.reason = StatusCode::Unsupported;
    decision.rationale = "the active policy does not define enforceable read semantics";
    state.record_decision(DecisionKind::ReadAcquire, context, StatusCode::Unsupported,
                          decision.rationale);
    return Result<ReadDecision>::success(decision);
  }

  decision.authoritative_version = resolved.object->authoritative_version;
  if (!decision.authoritative_version.defined()) {
    // Nothing has ever been published, so no replica can be certified as
    // matching an authoritative version. A live observation proves what a
    // replica holds; it does not create an authoritative version.
    decision.outcome = ReadOutcome::ReadBlocked;
    decision.reason = StatusCode::NotAuthoritative;
    decision.rationale =
        "the object has no authoritative version yet, so no replica can be certified as current; "
        "an authoritative version must be published first";
    decision.region = select_read_region_locked(state, *resolved.object, request.region);
    decision.region_state = CoherenceState::Unknown;
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }
  decision.region = select_read_region_locked(state, *resolved.object, request.region);
  if (!decision.region.defined()) {
    decision.outcome = ReadOutcome::ReadBlocked;
    decision.reason = StatusCode::UnknownRegion;
    decision.rationale = "the object has no replica that can supply contents";
    decision.region_state = CoherenceState::Unknown;
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }
  RegionRecord* region = nullptr;
  Status resolved_region = resolve_region_locked(state, resolved, decision.region,
                                                 request.region_generation, region);
  if (!resolved_region.ok()) {
    decision.outcome = ReadOutcome::ReadBlocked;
    decision.reason = resolved_region.code();
    decision.rationale = resolved_region.to_string();
    decision.region_state = CoherenceState::Unknown;
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }

  decision.region_generation = region->generation;
  decision.region_state = region->state;
  decision.region_version = region->version;
  decision.evidence_class = region->evidence_class;
  decision.evidence = region->evidence;
  decision.evidence_generation = region->evidence_generation;

  if (decision.authoritative_version.defined() && region->version.defined() &&
      region->version <= decision.authoritative_version) {
    decision.staleness_known = true;
    decision.staleness = decision.authoritative_version.value() - region->version.value();
  }

  const FreshnessAssessment freshness = assess_region_locked(state, *policy, *region);
  decision.evidence_fresh = freshness.fresh();

  const bool stale_permitted = [&] {
    switch (policy->stale_read_policy) {
      case StaleReadPolicy::Always:
        return true;
      case StaleReadPolicy::Bounded:
        return decision.staleness_known && decision.staleness <= policy->stale_read_bound_versions;
      case StaleReadPolicy::Never:
      case StaleReadPolicy::Unspecified:
      default:
        return false;
    }
  }();

  const bool current_by_state = is_current_state(region->state);
  const bool current_by_version = !decision.authoritative_version.defined() ||
                                  region->version == decision.authoritative_version;

  if (policy->consistency == ConsistencyModel::Snapshot) {
    if (!request.snapshot_version.defined()) {
      decision.outcome = ReadOutcome::ReadBlocked;
      decision.reason = StatusCode::InvalidArgument;
      decision.rationale =
          "snapshot consistency requires an explicit snapshot version in the read request";
      state.record_decision(DecisionKind::ReadAcquire, context, decision.reason,
                            decision.rationale);
      return Result<ReadDecision>::success(decision);
    }
    const VersionId snapshot = request.snapshot_version;
    if (snapshot > resolved.object->authoritative_version) {
      decision.outcome = ReadOutcome::ReadBlocked;
      decision.reason = StatusCode::StalePublication;
      decision.rationale =
          "the requested snapshot version has not been published; a snapshot cannot bind to a "
          "version that never became authoritative";
      state.record_decision(DecisionKind::ReadAcquire, context, decision.reason,
                            decision.rationale);
      return Result<ReadDecision>::success(decision);
    }
    if (region->version.defined() && region->version >= snapshot &&
        (current_by_state || region->state == CoherenceState::Stale)) {
      decision.outcome = ReadOutcome::ReadCurrent;
      decision.authoritative_version = snapshot;
      decision.reason = StatusCode::Ok;
      decision.staleness = region->version.value() - snapshot.value();
      decision.staleness_known = true;
      decision.rationale = "replica version " + region->version.to_string() +
                           " satisfies snapshot version " + snapshot.to_string() +
                           "; the authoritative version reported is the snapshot, not the newest "
                           "version";
      state.record_decision(DecisionKind::ReadAcquire, context, StatusCode::Ok,
                            decision.rationale);
      return Result<ReadDecision>::success(decision);
    }
    decision.outcome = ReadOutcome::ReadAfterSync;
    decision.sync_required = true;
    decision.reason = StatusCode::SyncRequired;
    decision.rationale =
        "replica predates the requested snapshot version; synchronization is required";
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }

  if (current_by_state && current_by_version && freshness.fresh()) {
    decision.outcome = ReadOutcome::ReadCurrent;
    decision.reason = StatusCode::Ok;
    decision.rationale =
        "replica matches the authoritative version and carries live evidence under the current "
        "coordinator epoch and participant boot";
    if (create_lease) {
      state.tick();
      const LeaseId lease = LeaseId::from_value(state.next_lease_id++);
      add_read_grant_locked(state, *resolved.object, *region, request.context.participant,
                            request.context.boot, lease);
      for (const ReadGrant& grant : resolved.object->reads) {
        if (!grant.released && grant.participant == request.context.participant &&
            grant.boot == request.context.boot && grant.region == region->id &&
            grant.version == region->version) {
          decision.lease = grant.lease;
          break;
        }
      }
    }
    state.record_decision(DecisionKind::ReadAcquire, context, StatusCode::Ok, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }

  if (stale_permitted && is_readable_state(region->state)) {
    decision.outcome = ReadOutcome::ReadStaleAllowed;
    decision.stale_allowed = true;
    decision.reason = StatusCode::ReadNotCurrent;
    decision.rationale =
        "policy permits stale reads: region version " + region->version.to_string() +
        " is the latest known value, authoritative version is " +
        decision.authoritative_version.to_string() +
        (decision.staleness_known ? " (staleness " + std::to_string(decision.staleness) + ")"
                                  : std::string(" (staleness unknown)"));
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }

  if (region->state == CoherenceState::Retired) {
    decision.outcome = ReadOutcome::ReadBlocked;
    decision.reason = StatusCode::Retired;
    decision.rationale = "the selected replica has been retired and holds no current contents";
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }
  if (region->state == CoherenceState::Fenced) {
    decision.outcome = ReadOutcome::ReadBlocked;
    decision.reason = StatusCode::Fenced;
    decision.rationale = "the selected replica belongs to a fenced participant incarnation";
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }
  if (!freshness.fresh() && freshness.verdict == FreshnessVerdict::Unsupported) {
    decision.outcome = ReadOutcome::Unsupported;
    decision.reason = StatusCode::Unsupported;
    decision.rationale = freshness.rationale;
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }

  // A recoverable source is another replica that is current. The cached
  // current replica answers this without walking the replica list whenever it is
  // a different replica; otherwise the list is scanned once.
  bool recoverable_source = false;
  const RegionId cached = resolved.object->preferred_current_replica;
  if (cached.defined() && cached != region->id) {
    const auto it = state.regions.find(cached);
    recoverable_source = it != state.regions.end() && is_current_state(it->second.state);
  }
  if (!recoverable_source) {
    for (const RegionId id : resolved.object->replicas) {
      if (id == region->id || id == cached) continue;
      const auto it = state.regions.find(id);
      if (it == state.regions.end()) continue;
      if (it->second.state == CoherenceState::Current) {
        resolved.object->preferred_current_replica = id;
        recoverable_source = true;
        break;
      }
    }
  }

  if (recoverable_source) {
    decision.outcome = ReadOutcome::ReadAfterSync;
    decision.sync_required = true;
    decision.reason = freshness.fresh() ? StatusCode::SyncRequired : freshness.reason;
    decision.rationale = "the replica cannot satisfy a current read: state=" +
                         std::string(to_token(region->state)) + "; " + freshness.rationale +
                         "; a current source replica exists, so an explicit synchronization can "
                         "establish currentness";
    state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
    return Result<ReadDecision>::success(decision);
  }

  decision.outcome = ReadOutcome::ReadBlocked;
  decision.reason = freshness.fresh() ? StatusCode::ReadNotCurrent : freshness.reason;
  decision.rationale = "the read is blocked: replica state=" +
                       std::string(to_token(region->state)) + ", version=" +
                       region->version.to_string() + ", authoritative version=" +
                       decision.authoritative_version.to_string() + "; " + freshness.rationale;
  state.record_decision(DecisionKind::ReadAcquire, context, decision.reason, decision.rationale);
  return Result<ReadDecision>::success(decision);
}

} // namespace

Result<ReadDecision> CoherenceEngine::acquire_read(const ReadRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto outcome = compute_read_locked(*impl_, request, true);
  if (outcome.has_value()) (void)impl_->maybe_compact_locked();
  return outcome;
}

Result<ReadDecision> CoherenceEngine::explain_read(const ReadRequest& request) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return compute_read_locked(*impl_, request, false);
}

// ---------------------------------------------------------------------------
// Write authority
// ---------------------------------------------------------------------------
Result<WriteGrant> CoherenceEngine::acquire_write(const WriteRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Resolved resolved;
  Status authority = resolve_locked(*impl_, request.context, resolved);
  const DecisionContext context = impl_->make_context(
      request.context, request.context.object, request.context.object_generation,
      resolved.object != nullptr ? resolved.object->ownership_generation
                                 : OwnershipGeneration::nil());
  WriteGrant grant;
  grant.context = context;
  grant.writer = request.context.participant;
  grant.writer_boot = request.context.boot;

  if (!authority.ok()) {
    grant.granted = false;
    grant.reason = authority.code();
    grant.rationale = authority.to_string();
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::failure(authority);
  }
  ObjectRecord& object = *resolved.object;
  grant.writer_boot = resolved.participant->boot;
  grant.base_version = object.authoritative_version;
  grant.authoritative_version = object.authoritative_version;
  grant.reason = StatusCode::Ok;

  const CoherencePolicy* policy = policy_locked(*impl_, object);
  if (policy == nullptr) {
    grant.reason = StatusCode::UnknownPolicy;
    grant.rationale = "the object references a policy that is not present";
    return Result<WriteGrant>::failure(Status(grant.reason, grant.rationale));
  }
  if (policy->write_ownership == WriteOwnershipMode::ReadOnlyObject) {
    grant.reason = StatusCode::NotWriteAuthorized;
    grant.rationale = "the active policy declares this object read-only";
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }
  if (policy->write_ownership != WriteOwnershipMode::SingleWriterExclusive) {
    grant.reason = StatusCode::Unsupported;
    grant.rationale = "the active policy does not declare single-writer exclusive authority";
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }
  if (object.publication_state == PublicationState::PendingDurable) {
    grant.reason = StatusCode::WriteConflict;
    grant.rationale = "a publication for this object is pending durability";
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }
  if (object.has_unpublished_dirty) {
    grant.reason = StatusCode::DirtyUnpublished;
    grant.rationale =
        "the object carries an unpublished modification; the outcome of the previous writer must "
        "be resolved before new write authority is granted";
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }

  RegionRecord* writer_region = nullptr;
  if (request.region.defined()) {
    Status resolved_region = resolve_region_locked(*impl_, resolved, request.region,
                                                   request.region_generation, writer_region);
    if (!resolved_region.ok()) {
      grant.reason = resolved_region.code();
      grant.rationale = resolved_region.to_string();
      impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
      return Result<WriteGrant>::success(grant);
    }
    if (writer_region->participant != request.context.participant) {
      grant.reason = StatusCode::NotWriteAuthorized;
      grant.rationale = "the writer does not own the region it intends to mutate";
      impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
      return Result<WriteGrant>::success(grant);
    }
    grant.writer_region = writer_region->id;
    grant.writer_region_generation = writer_region->generation;
  }

  if (object.authority == AuthorityMode::ExclusiveWriter &&
      object.writer == request.context.participant && object.writer_boot == request.context.boot) {
    grant.granted = true;
    grant.may_mutate_now = true;
    grant.base_version = object.authoritative_version;
    grant.rationale = "the caller already holds exclusive write authority for this generation";
    impl_->record_decision(DecisionKind::WriteAcquire, context, StatusCode::Ok, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }
  if (object.authority == AuthorityMode::TransferPending) {
    const bool same_writer = object.writer == request.context.participant &&
                             object.writer_boot == request.context.boot;
    if (!same_writer) {
      grant.reason = StatusCode::ExclusiveWriterConflict;
      grant.rationale =
          "another participant holds exclusive write authority for this object generation; "
          "single-writer policy forbids two concurrent writers";
      impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
      return Result<WriteGrant>::success(grant);
    }
    if (!impl_->has_outstanding_invalidations_locked(object.id)) {
      object.authority = AuthorityMode::ExclusiveWriter;
      JournalEntry entry;
      entry.kind = JournalEntryKind::UpsertObject;
      entry.object = object;
      scrub_object_for_persistence(entry.object);
      Status written = impl_->append_locked(entry);
      if (!written.ok()) return Result<WriteGrant>::failure(written);
      grant.granted = true;
      grant.may_mutate_now = true;
      grant.reason = StatusCode::Ok;
      grant.rationale = "write authority is now effective: every mandatory invalidation settled";
      impl_->record_decision(DecisionKind::WriteAcquire, context, StatusCode::Ok, grant.rationale);
      return Result<WriteGrant>::success(grant);
    }
    grant.granted = true;
    grant.may_mutate_now = false;
    grant.reason = StatusCode::InvalidationOutstanding;
    grant.rationale = "mandatory invalidations are still outstanding for this write transfer";
    for (const InvalidationId id : object.outstanding_invalidations) {
      const auto it = impl_->invalidations.find(id);
      if (it != impl_->invalidations.end() && it->second.state == InvalidationState::Requested) {
        grant.required_invalidations.push_back(it->second);
      }
    }
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }
  if (object.authority == AuthorityMode::ExclusiveWriter &&
      (object.writer != request.context.participant ||
       object.writer_boot != request.context.boot)) {
    grant.reason = StatusCode::ExclusiveWriterConflict;
    grant.rationale =
        "another participant holds exclusive write authority for this object generation; "
        "single-writer policy forbids two concurrent writers";
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }

  if (writer_region != nullptr && !is_writable_state(writer_region->state)) {
    grant.reason = writer_region->state == CoherenceState::RevalidationRequired
                       ? StatusCode::RevalidationRequired
                       : StatusCode::SyncRequired;
    grant.rationale =
        "the writer's replica is not current: state=" +
        std::string(to_token(writer_region->state)) +
        "; revalidate or synchronize the replica before acquiring write authority";
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }

  std::vector<InvalidationRecord> required;
  for (const RegionId id : object.replicas) {
    const auto it = impl_->regions.find(id);
    if (it == impl_->regions.end()) continue;
    RegionRecord& region = it->second;
    if (region.id == grant.writer_region) continue;
    if (!is_live_state(region.state)) continue;
    if (region.state == CoherenceState::RevalidationRequired) continue;
    required.push_back(InvalidationRecord{});
    required.back().region = region.id;
    required.back().region_generation = region.generation;
    required.back().replica_generation = region.replica_generation;
  }

  if (!required.empty() && policy->conflict_behavior == ConflictBehavior::Reject) {
    grant.reason = StatusCode::WriteConflict;
    grant.rationale =
        "incompatible replicas exist and the policy forbids invalidating them automatically";
    impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
    return Result<WriteGrant>::success(grant);
  }

  impl_->tick();
  const OwnershipGeneration next_ownership = object.ownership_generation.next();
  object.ownership_generation = next_ownership;
  object.writer = request.context.participant;
  object.writer_boot = resolved.participant->boot;
  object.authority =
      required.empty() ? AuthorityMode::ExclusiveWriter : AuthorityMode::TransferPending;
  object.updated_sequence = impl_->sequence;
  grant.context.ownership_generation = next_ownership;
  grant.granted = true;
  grant.may_mutate_now = required.empty();
  grant.base_version = object.authoritative_version;

  std::vector<JournalEntry> entries;
  for (ReadGrant& read : object.reads) {
    if (!read.released) read.released = true;
  }
  for (InvalidationRecord& invalidation : required) {
    invalidation.id = InvalidationId::from_value(impl_->next_invalidation_id++);
    invalidation.object = object.id;
    invalidation.object_generation = object.generation;
    invalidation.published_version = object.authoritative_version;
    invalidation.ownership_generation = next_ownership;
    invalidation.epoch = impl_->epoch;
    invalidation.state = InvalidationState::Requested;
    invalidation.acknowledgement_required = true;
    invalidation.issued_sequence = impl_->sequence;
    invalidation.rationale = "superseded by a write transfer";
    const auto region_it = impl_->regions.find(invalidation.region);
    if (region_it != impl_->regions.end()) {
      invalidation.target_participant = region_it->second.participant;
      invalidation.target_boot = region_it->second.boot;
      invalidation.superseded_version = region_it->second.version;
    }
    object.outstanding_invalidations.push_back(invalidation.id);
    grant.required_invalidations.push_back(invalidation);
  }
  std::sort(object.outstanding_invalidations.begin(), object.outstanding_invalidations.end());

  if (required.empty()) {
    grant.reason = StatusCode::Ok;
    grant.rationale = "exclusive write authority granted; no incompatible replicas were present";
  } else {
    grant.reason = StatusCode::InvalidationOutstanding;
    grant.rationale = "write authority reserved pending " + std::to_string(required.size()) +
                      " mandatory invalidation acknowledgement(s)";
  }

  engine_detail::push_object_entry(entries, object);
  for (const InvalidationRecord& invalidation : required) {
    engine_detail::push_invalidation_entry(entries, invalidation);
  }
  Status written = impl_->append_many_locked(entries);
  if (!written.ok()) {
    // Roll the reservation back so in-memory state never claims authority that
    // the durable record does not contain.
    object.ownership_generation = OwnershipGeneration::from_value(next_ownership.value() - 1);
    object.authority = AuthorityMode::None;
    object.writer = ParticipantId::nil();
    object.writer_boot = ParticipantBootId::nil();
    object.outstanding_invalidations.clear();
    grant.granted = false;
    grant.reason = written.code();
    grant.rationale = written.to_string();
    return Result<WriteGrant>::failure(written);
  }

  for (const InvalidationRecord& invalidation : required) {
    impl_->invalidations[invalidation.id] = invalidation;
    const auto region_it = impl_->regions.find(invalidation.region);
    if (region_it != impl_->regions.end()) {
      region_it->second.invalidation_pending = true;
      region_it->second.pending_invalidation = invalidation.id;
      region_it->second.pending_invalidation_target = region_it->second.generation;
      region_it->second.updated_sequence = impl_->sequence;
    }
  }
  impl_->record_decision(DecisionKind::WriteAcquire, context, grant.reason, grant.rationale);
  return Result<WriteGrant>::success(grant);
}

} // namespace coherence
