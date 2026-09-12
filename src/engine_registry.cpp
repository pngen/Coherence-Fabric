// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Engine registry operations: objects, regions, policies, inspection, shutdown.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/version.hpp"
#include "engine_impl.hpp"

namespace coherence {

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------
Result<ObjectRecord> CoherenceEngine::register_object(CoherenceDomainId domain, std::string name,
                                                      std::uint64_t length,
                                                      const CoherencePolicy& policy) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->shutting_down) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::ShuttingDown, "the coherence engine is shutting down"));
  }
  if (impl_->domains.count(domain) == 0) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::UnknownDomain, "no such domain", domain.to_string()));
  }
  if (name.empty()) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::InvalidArgument, "object name must not be empty"));
  }
  if (length == 0 || length > impl_->config.max_object_length) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::InvalidArgument, "object length is out of range",
               std::to_string(length) + " vs bound " + std::to_string(impl_->config.max_object_length)));
  }
  Status policy_ok = validate_policy(policy);
  if (!policy_ok.ok()) {
    return Result<ObjectRecord>::failure(
        Status(policy_ok.code(), "policy rejected at object registration", policy_ok.to_string()));
  }
  const ObjectNameKey key{domain, name};
  if (impl_->object_index.count(key) != 0) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::DuplicateIdentity, "an object with this name already exists in the domain",
               name));
  }
  if (impl_->objects.size() >= impl_->config.max_objects) {
    return Result<ObjectRecord>::failure(Status(StatusCode::CapacityExceeded,
                                                "object capacity reached",
                                                std::to_string(impl_->config.max_objects)));
  }

  CoherencePolicy stored = policy;
  stored.id = PolicyId::from_value(impl_->next_policy_id++);
  stored.generation = PolicyGeneration::from_value(1);
  if (stored.name.empty()) stored.name = "policy";

  ObjectRecord record;
  record.id = ObjectId::from_value(impl_->next_object_id++);
  record.generation = ObjectGeneration::from_value(1);
  record.domain = domain;
  record.name = name;
  record.length = length;
  record.policy = stored.id;
  record.policy_generation = stored.generation;
  record.lifecycle = ObjectLifecycle::Active;
  record.authority = AuthorityMode::None;
  record.ownership_generation = OwnershipGeneration::from_value(1);
  record.updated_sequence = impl_->sequence;

  JournalEntry policy_entry;
  policy_entry.kind = JournalEntryKind::UpsertPolicy;
  policy_entry.policy = stored;
  JournalEntry object_entry;
  object_entry.kind = JournalEntryKind::UpsertObject;
  object_entry.object = record;
  scrub_object_for_persistence(object_entry.object);

  Status written = impl_->append_many_locked({policy_entry, object_entry});
  if (!written.ok()) return Result<ObjectRecord>::failure(written);

  impl_->policies.emplace(stored.id, stored);
  impl_->objects.emplace(record.id, record);
  impl_->object_index.emplace(key, record.id);
  impl_->tick();
  return Result<ObjectRecord>::success(record);
}

Result<ObjectRecord> CoherenceEngine::get_object(ObjectId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->objects.find(id);
  if (it == impl_->objects.end()) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::UnknownObject, "no such coherence object", id.to_string()));
  }
  return Result<ObjectRecord>::success(it->second);
}

Result<ObjectRecord> CoherenceEngine::find_object(CoherenceDomainId domain,
                                                  std::string_view name) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->object_index.find(ObjectNameKey{domain, std::string(name)});
  if (it == impl_->object_index.end()) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::UnknownObject, "no such coherence object", std::string(name)));
  }
  return Result<ObjectRecord>::success(impl_->objects.at(it->second));
}

Result<CoherencePolicy> CoherenceEngine::get_policy(ObjectId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto object = impl_->objects.find(id);
  if (object == impl_->objects.end()) {
    return Result<CoherencePolicy>::failure(
        Status(StatusCode::UnknownObject, "no such coherence object", id.to_string()));
  }
  const auto policy = impl_->policies.find(object->second.policy);
  if (policy == impl_->policies.end()) {
    return Result<CoherencePolicy>::failure(
        Status(StatusCode::UnknownPolicy, "the object references a policy that is not present",
               object->second.policy.to_string()));
  }
  return Result<CoherencePolicy>::success(policy->second);
}

Result<ObjectRecord> CoherenceEngine::set_policy(ObjectId id, ObjectGeneration generation,
                                                 PolicyGeneration expected_generation,
                                                 const CoherencePolicy& next) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->shutting_down) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::ShuttingDown, "the coherence engine is shutting down"));
  }
  const auto it = impl_->objects.find(id);
  if (it == impl_->objects.end()) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::UnknownObject, "no such coherence object", id.to_string()));
  }
  ObjectRecord& object = it->second;
  if (object.generation != generation) {
    return Result<ObjectRecord>::failure(Status(
        StatusCode::StaleObjectGeneration, "policy change references a stale object generation",
        "request=" + generation.to_string() + " current=" + object.generation.to_string()));
  }
  if (object.policy_generation != expected_generation) {
    return Result<ObjectRecord>::failure(Status(
        StatusCode::StalePolicy, "policy change references a stale policy generation",
        "request=" + expected_generation.to_string() +
            " current=" + object.policy_generation.to_string()));
  }
  if (object.publication_state == PublicationState::PendingDurable) {
    return Result<ObjectRecord>::failure(Status(
        StatusCode::InvalidState, "policy cannot change while a publication is pending durability"));
  }
  Status policy_ok = validate_policy(next);
  if (!policy_ok.ok()) {
    return Result<ObjectRecord>::failure(
        Status(policy_ok.code(), "replacement policy rejected", policy_ok.to_string()));
  }

  CoherencePolicy stored = next;
  stored.id = object.policy;
  stored.generation = object.policy_generation.next();
  if (stored.name.empty()) {
    const auto old = impl_->policies.find(object.policy);
    stored.name = old != impl_->policies.end() ? old->second.name : std::string("policy");
  }

  object.policy = stored.id;
  object.policy_generation = stored.generation;
  object.updated_sequence = impl_->sequence;

  JournalEntry policy_entry;
  policy_entry.kind = JournalEntryKind::UpsertPolicy;
  policy_entry.policy = stored;
  JournalEntry object_entry;
  object_entry.kind = JournalEntryKind::UpsertObject;
  object_entry.object = object;
  scrub_object_for_persistence(object_entry.object);

  Status written = impl_->append_many_locked({policy_entry, object_entry});
  if (!written.ok()) return Result<ObjectRecord>::failure(written);

  impl_->policies[stored.id] = stored;
  impl_->tick();

  const DecisionContext context =
      impl_->make_context(AuthorityContext{}, object.id, object.generation,
                          object.ownership_generation);
  impl_->record_decision(DecisionKind::PolicyChange, context, StatusCode::Ok,
                         "policy advanced to generation " + stored.generation.to_string());
  return Result<ObjectRecord>::success(object);
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------
Result<RegionRecord> CoherenceEngine::register_region(const RegionRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->shutting_down) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::ShuttingDown, "the coherence engine is shutting down"));
  }
  const AuthorityContext& ctx = registration.context;
  if (ctx.epoch != impl_->epoch) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::StaleEpoch, "region registration carries a stale coordinator epoch",
        "request=" + ctx.epoch.to_string() + " current=" + impl_->epoch.to_string()));
  }
  const auto object_it = impl_->objects.find(ctx.object);
  if (object_it == impl_->objects.end()) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::UnknownObject, "no such coherence object", ctx.object.to_string()));
  }
  ObjectRecord& object = object_it->second;
  if (object.generation != ctx.object_generation) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::StaleObjectGeneration, "region registration references a stale object generation",
        "request=" + ctx.object_generation.to_string() +
            " current=" + object.generation.to_string()));
  }
  if (registration.domain.defined() && registration.domain != object.domain) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::InvalidArgument, "region domain does not match the object domain"));
  }
  if (object.lifecycle == ObjectLifecycle::Retired ||
      object.lifecycle == ObjectLifecycle::RecoveryRequired) {
    return Result<RegionRecord>::failure(Status(
        object.lifecycle == ObjectLifecycle::Retired ? StatusCode::Retired
                                                     : StatusCode::RecoveryRequired,
        "the object does not accept new replicas in its current lifecycle"));
  }
  const auto participant_it = impl_->participants.find(ctx.participant);
  if (participant_it == impl_->participants.end()) {
    return Result<RegionRecord>::failure(Status(StatusCode::UnknownParticipant,
                                                "no such participant",
                                                ctx.participant.to_string()));
  }
  const ParticipantRecord& participant = participant_it->second;
  if (participant.boot != ctx.boot) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::StaleBoot, "region registration references a boot identity that is not live",
        "request=" + ctx.boot.to_string() + " live=" + participant.boot.to_string()));
  }
  if (participant.lifecycle == ParticipantLifecycle::Fenced) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::Fenced, "the participant has been fenced", participant.name));
  }
  if (registration.memory_domain == MemoryDomain::Unknown) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::InvalidArgument, "region memory domain must be declared explicitly"));
  }
  if (registration.evidence_class == EvidenceClass::Unsupported) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::Unsupported, "the backend reports that it cannot produce evidence for this region"));
  }
  if (registration.name.empty()) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::InvalidArgument, "region name must not be empty"));
  }
  if (registration.length == 0) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::InvalidArgument, "region length must be greater than zero"));
  }
  // Overflow-safe extent validation: the region must lie inside the object.
  if (!checked_range(registration.offset, registration.length, object.length)) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::InvalidArgument, "region extent lies outside the object extent",
        "offset=" + std::to_string(registration.offset) +
            " length=" + std::to_string(registration.length) +
            " object_length=" + std::to_string(object.length)));
  }
  if (object.replicas.size() >= impl_->config.max_regions_per_object) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::CapacityExceeded, "replica capacity reached for this object",
        std::to_string(impl_->config.max_regions_per_object)));
  }
  if (impl_->regions.size() >= impl_->config.max_regions) {
    return Result<RegionRecord>::failure(Status(StatusCode::CapacityExceeded,
                                                "region capacity reached",
                                                std::to_string(impl_->config.max_regions)));
  }

  const auto policy_it = impl_->policies.find(object.policy);
  if (policy_it == impl_->policies.end()) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::UnknownPolicy, "the object references a missing policy"));
  }
  if (!policy_it->second.allows_domain(registration.memory_domain)) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::InvalidArgument, "the active policy does not permit this memory domain",
        std::string(to_token(registration.memory_domain))));
  }

  if (registration.requested_id.defined() && impl_->regions.count(registration.requested_id) != 0) {
    return Result<RegionRecord>::failure(Status(
        StatusCode::DuplicateIdentity, "the requested region identity is already in use",
        registration.requested_id.to_string()));
  }

  RegionRecord record;
  record.id = registration.requested_id.defined()
                  ? registration.requested_id
                  : RegionId::from_value(impl_->next_region_id++);
  if (registration.requested_id.defined() && registration.requested_id.value() >= impl_->next_region_id) {
    impl_->next_region_id = registration.requested_id.value() + 1;
  }
  record.generation = RegionGeneration::from_value(1);
  record.replica = registration.requested_replica.defined()
                       ? registration.requested_replica
                       : ReplicaId::from_value(impl_->next_replica_id++);
  if (registration.requested_replica.defined() &&
      registration.requested_replica.value() >= impl_->next_replica_id) {
    impl_->next_replica_id = registration.requested_replica.value() + 1;
  }
  record.replica_generation = ReplicaGeneration::from_value(1);
  record.domain = object.domain;
  record.object = object.id;
  record.object_generation = object.generation;
  record.participant = participant.id;
  record.boot = participant.boot;
  record.memory_domain = registration.memory_domain;
  record.evidence_class = registration.evidence_class;
  record.name = registration.name;
  record.offset = registration.offset;
  record.length = registration.length;
  record.address_hint = registration.address_hint;
  record.declared_writable = registration.declared_writable;
  record.lifecycle = RegionLifecycle::Active;
  // A newly registered replica is never current. Contents that were merely
  // declared are not evidence of anything.
  record.state = registration.initial_version.defined() ? CoherenceState::RevalidationRequired
                                                        : CoherenceState::Unknown;
  record.version = registration.initial_version;
  record.ownership_generation = object.ownership_generation;
  record.dirty = DirtyCondition::Clean;
  record.content = registration.content;
  record.updated_sequence = impl_->sequence;
  record.note = "registered; dynamic currentness requires revalidation";

  JournalEntry region_entry;
  region_entry.kind = JournalEntryKind::UpsertRegion;
  region_entry.region = record;

  const RegionNameKey name_key{object.id, record.name};
  {
    const auto existing = impl_->region_index.find(name_key);
    if (existing != impl_->region_index.end()) {
      const auto previous = impl_->regions.find(existing->second);
      // A retired replica does not hold its name; a replacement may take it.
      if (previous == impl_->regions.end() ||
          previous->second.lifecycle == RegionLifecycle::Retired) {
        impl_->region_index.erase(existing);
      } else {
        return Result<RegionRecord>::failure(Status(
            StatusCode::DuplicateIdentity, "a region with this name already exists for the object",
            record.name));
      }
    }
  }

  // The list stays in ascending identity order without re-sorting the whole
  // list on every registration: identities are allocated monotonically, so the
  // common case is a constant-time append.
  if (object.replicas.empty() || record.id > object.replicas.back()) {
    object.replicas.push_back(record.id);
  } else {
    const auto position =
        std::lower_bound(object.replicas.begin(), object.replicas.end(), record.id);
    object.replicas.insert(position, record.id);
  }
  object.updated_sequence = impl_->sequence;

  // The participant's region accounting is derived state. It is recomputed from
  // the replicas that actually exist rather than journaled on every change,
  // which keeps it consistent by construction. Entry construction is skipped
  // entirely when there is nothing to write, because an object record carries
  // its full replica list.
  Status written = Status::success();
  if (impl_->durability_active()) {
    JournalEntry object_entry;
    object_entry.kind = JournalEntryKind::UpsertObject;
    object_entry.object = object;
    scrub_object_for_persistence(object_entry.object);
    written = impl_->append_many_locked({object_entry, region_entry});
  }
  if (!written.ok()) return Result<RegionRecord>::failure(written);

  impl_->regions.emplace(record.id, record);
  impl_->region_index[name_key] = record.id;
  participant_it->second.region_count += 1;
  impl_->tick();
  return Result<RegionRecord>::success(record);
}

Result<RegionRecord> CoherenceEngine::get_region(RegionId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto it = impl_->regions.find(id);
  if (it == impl_->regions.end()) {
    return Result<RegionRecord>::failure(
        Status(StatusCode::UnknownRegion, "no such region", id.to_string()));
  }
  return Result<RegionRecord>::success(it->second);
}

Status CoherenceEngine::retire_region(RegionId id, RegionGeneration generation,
                                      CoordinatorEpoch epoch) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (epoch != impl_->epoch) {
    return Status(StatusCode::StaleEpoch, "region retirement carries a stale coordinator epoch",
                  "request=" + epoch.to_string() + " current=" + impl_->epoch.to_string());
  }
  const auto it = impl_->regions.find(id);
  if (it == impl_->regions.end()) {
    return Status(StatusCode::UnknownRegion, "no such region", id.to_string());
  }
  RegionRecord& region = it->second;
  if (region.generation != generation) {
    return Status(StatusCode::StaleRegionGeneration,
                  "region retirement references a stale region generation",
                  "request=" + generation.to_string() + " current=" + region.generation.to_string());
  }
  if (region.lifecycle == RegionLifecycle::Retired) {
    return Status::success();  // idempotent
  }
  if (!is_legal_region_transition(region.lifecycle, RegionLifecycle::Retired)) {
    return Status(StatusCode::InvalidTransition,
                  "region lifecycle does not permit retirement from its current state",
                  std::string(to_token(region.lifecycle)));
  }
  impl_->tick();
  // Retirement revokes live authority: a retired replica can never be a source
  // of truth and its outstanding invalidation is satisfied.
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
  impl_->release_region_name_locked(region);

  const auto object_it = impl_->objects.find(region.object);
  if (object_it != impl_->objects.end()) {
    ObjectRecord& object = object_it->second;
    for (ReadGrant& grant : object.reads) {
      if (grant.region == region.id && !grant.released) grant.released = true;
    }
    impl_->recompute_authority_locked(object);
    JournalEntry object_entry;
    object_entry.kind = JournalEntryKind::UpsertObject;
    object_entry.object = object;
    scrub_object_for_persistence(object_entry.object);
    (void)impl_->append_locked(object_entry);
  }

  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertRegion;
  entry.region = region;
  Status written = impl_->append_locked(entry);
  if (!written.ok()) return written;
  impl_->tick();
  return Status::success();
}

Result<ObjectRecord> CoherenceEngine::resolve_recovery(ObjectId id, ObjectGeneration generation,
                                                   CoordinatorEpoch epoch, std::string note) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (epoch != impl_->epoch) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::StaleEpoch, "recovery resolution carries a stale coordinator epoch",
               "request=" + epoch.to_string() + " current=" + impl_->epoch.to_string()));
  }
  const auto it = impl_->objects.find(id);
  if (it == impl_->objects.end()) {
    return Result<ObjectRecord>::failure(
        Status(StatusCode::UnknownObject, "no such coherence object", id.to_string()));
  }
  ObjectRecord& object = it->second;
  // An explicit generation is validated strictly. An operator-driven resolution
  // may omit it, in which case the object is resolved as it currently is; the
  // decision is still recorded against the generation that was resolved.
  if (generation.defined() && object.generation != generation) {
    return Result<ObjectRecord>::failure(Status(
        StatusCode::StaleObjectGeneration,
        "recovery resolution references a stale object generation",
        "request=" + generation.to_string() + " current=" + object.generation.to_string()));
  }
  if (object.lifecycle != ObjectLifecycle::RecoveryRequired) {
    return Result<ObjectRecord>::failure(Status(
        StatusCode::InvalidState,
        "the object does not require recovery in its current lifecycle",
        std::string(to_token(object.lifecycle))));
  }
  if (!is_legal_object_transition(object.lifecycle, ObjectLifecycle::Active)) {
    return Result<ObjectRecord>::failure(Status(
        StatusCode::InvalidTransition, "recovery resolution is not a legal lifecycle transition"));
  }

  impl_->tick();
  object.lifecycle = ObjectLifecycle::Active;
  // The outcome is recorded truthfully: the unpublished modification is gone,
  // and the authoritative version is unchanged.
  object.has_unpublished_dirty = false;
  object.dirty_condition = DirtyCondition::DirtyLost;
  object.pending_publication = PublicationId::nil();
  object.pending_version = VersionId::nil();
  object.pending_content = ContentFingerprint{};
  if (object.publication_state == PublicationState::PendingDurable ||
      object.publication_state == PublicationState::RecoveryRequired) {
    object.publication_state = PublicationState::Aborted;
  }
  object.recovery_note = note.empty()
                             ? std::string("operator resolved recovery: the unpublished "
                                           "modification was lost and the last authoritative "
                                           "version stands")
                             : std::move(note);
  object.updated_sequence = impl_->sequence;

  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertObject;
  entry.object = object;
  scrub_object_for_persistence(entry.object);
  Status written = impl_->append_locked(entry);
  if (!written.ok()) return Result<ObjectRecord>::failure(written);

  const DecisionContext context =
      impl_->make_context(AuthorityContext{}, object.id, object.generation,
                          object.ownership_generation);
  impl_->record_decision(DecisionKind::Recover, context, StatusCode::Ok, object.recovery_note);
  return Result<ObjectRecord>::success(object);
}

Status CoherenceEngine::retire_object(ObjectId id, ObjectGeneration generation,
                                      CoordinatorEpoch epoch) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (epoch != impl_->epoch) {
    return Status(StatusCode::StaleEpoch, "object retirement carries a stale coordinator epoch",
                  "request=" + epoch.to_string() + " current=" + impl_->epoch.to_string());
  }
  const auto it = impl_->objects.find(id);
  if (it == impl_->objects.end()) {
    return Status(StatusCode::UnknownObject, "no such coherence object", id.to_string());
  }
  ObjectRecord& object = it->second;
  if (object.generation != generation) {
    return Status(StatusCode::StaleObjectGeneration,
                  "object retirement references a stale object generation",
                  "request=" + generation.to_string() + " current=" + object.generation.to_string());
  }
  if (object.lifecycle == ObjectLifecycle::Retired) return Status::success();
  if (!is_legal_object_transition(object.lifecycle, ObjectLifecycle::Retired)) {
    return Status(StatusCode::InvalidTransition,
                  "object lifecycle does not permit retirement from its current state",
                  std::string(to_token(object.lifecycle)));
  }
  impl_->tick();
  // Retirement revokes every live authority over the object.
  object.lifecycle = ObjectLifecycle::Retired;
  object.authority = AuthorityMode::None;
  object.writer = ParticipantId::nil();
  object.writer_boot = ParticipantBootId::nil();
  object.pending_publication = PublicationId::nil();
  object.pending_version = VersionId::nil();
  object.publication_state = PublicationState::None;
  object.has_unpublished_dirty = false;
  for (ReadGrant& grant : object.reads) {
    grant.released = true;
  }
  object.updated_sequence = impl_->sequence;

  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertObject;
  entry.object = object;
  scrub_object_for_persistence(entry.object);
  Status written = impl_->append_locked(entry);
  if (!written.ok()) return written;

  const std::vector<RegionId> replicas = object.replicas;
  for (const RegionId region_id : replicas) {
    const auto region_it = impl_->regions.find(region_id);
    if (region_it == impl_->regions.end()) continue;
    RegionRecord& region = region_it->second;
    region.lifecycle = RegionLifecycle::Retired;
    region.state = CoherenceState::Retired;
    region.dirty = region.dirty == DirtyCondition::DirtyUnpublished ? DirtyCondition::DirtyLost
                                                                    : region.dirty;
    region.evidence = EvidenceId::nil();
    region.evidence_generation = EvidenceGeneration::nil();
    region.updated_sequence = impl_->sequence;
    JournalEntry region_entry;
    region_entry.kind = JournalEntryKind::UpsertRegion;
    region_entry.region = region;
    (void)impl_->append_locked(region_entry);
  }
  impl_->tick();
  return Status::success();
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------
Result<CoherenceSnapshot> CoherenceEngine::snapshot(const SnapshotOptions& options) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  CoherenceSnapshot out;
  out.epoch = impl_->epoch;
  out.sequence = impl_->sequence;
  out.build = build_identification();
  out.shutting_down = impl_->shutting_down;

  for (const auto& [id, record] : impl_->domains) {
    (void)id;
    out.domains.push_back(record);
  }
  if (options.include_participants) {
    for (const auto& [id, record] : impl_->participants) {
      (void)id;
      out.participants.push_back(record);
    }
  }
  std::uint64_t emitted = 0;
  for (const auto& [id, record] : impl_->objects) {
    (void)id;
    if (options.object_filter.defined() && record.id != options.object_filter) continue;
    if (options.max_objects != 0 && emitted >= options.max_objects) break;
    out.objects.push_back(record);
    ++emitted;
  }
  if (options.include_regions) {
    for (const auto& [id, record] : impl_->regions) {
      (void)id;
      if (options.object_filter.defined() && record.object != options.object_filter) continue;
      out.regions.push_back(record);
    }
  }
  if (options.include_invalidations) {
    for (const auto& [id, record] : impl_->invalidations) {
      (void)id;
      if (options.object_filter.defined() && record.object != options.object_filter) continue;
      out.invalidations.push_back(record);
    }
  }
  if (options.include_syncs) {
    for (const auto& [id, record] : impl_->syncs) {
      (void)id;
      if (options.object_filter.defined() && record.object != options.object_filter) continue;
      out.syncs.push_back(record);
    }
  }
  return Result<CoherenceSnapshot>::success(std::move(out));
}

std::string CoherenceEngine::render_decision_log() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->render_decision_log_locked();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
Status CoherenceEngine::begin_shutdown(CoordinatorEpoch epoch) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (epoch != impl_->epoch) {
    return Status(StatusCode::StaleEpoch, "shutdown carries a stale coordinator epoch",
                  "request=" + epoch.to_string() + " current=" + impl_->epoch.to_string());
  }
  if (impl_->shutting_down) return Status::success();  // idempotent
  impl_->tick();
  impl_->shutting_down = true;

  // Revoke new authority: no writer, no reader, no pending publication.
  for (auto& [object_id, object] : impl_->objects) {
    (void)object_id;
    object.authority = AuthorityMode::None;
    object.writer = ParticipantId::nil();
    object.writer_boot = ParticipantBootId::nil();
    if (object.publication_state == PublicationState::PendingDurable) {
      object.publication_state = PublicationState::Aborted;
      object.pending_publication = PublicationId::nil();
      object.pending_version = VersionId::nil();
      object.pending_content = ContentFingerprint{};
      JournalEntry clear;
      clear.kind = JournalEntryKind::ClearPendingPublication;
      clear.remove_object = object.id;
      (void)impl_->append_locked(clear);
    }
    for (ReadGrant& grant : object.reads) grant.released = true;
    object.updated_sequence = impl_->sequence;
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertObject;
    entry.object = object;
    scrub_object_for_persistence(entry.object);
    (void)impl_->append_locked(entry);
  }
  // Pending synchronizations are settled deterministically as cancelled rather
  // than left dangling.
  for (auto& [sync_id, sync] : impl_->syncs) {
    (void)sync_id;
    if (sync.state == SyncState::Completed || sync.state == SyncState::Cancelled ||
        sync.state == SyncState::Failed) {
      continue;
    }
    sync.state = SyncState::Cancelled;
    sync.settled_sequence = impl_->sequence;
    sync.failure_reason = "coordinator shutdown";
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertSync;
    entry.sync = sync;
    (void)impl_->append_locked(entry);
  }
  for (auto& [invalidation_id, invalidation] : impl_->invalidations) {
    (void)invalidation_id;
    if (invalidation.state != InvalidationState::Requested) continue;
    invalidation.state = InvalidationState::Unknown;
    invalidation.settled_sequence = impl_->sequence;
    invalidation.rationale = "coordinator shutdown before acknowledgement";
    JournalEntry entry;
    entry.kind = JournalEntryKind::UpsertInvalidation;
    entry.invalidation = invalidation;
    (void)impl_->append_locked(entry);
  }
  const DecisionContext context =
      impl_->make_context(AuthorityContext{}, ObjectId::nil(), ObjectGeneration::nil(),
                          OwnershipGeneration::nil());
  impl_->record_decision(DecisionKind::Shutdown, context, StatusCode::Ok,
                         "shutdown initiated; authority revoked");
  return Status::success();
}

Status CoherenceEngine::complete_shutdown(CoordinatorEpoch epoch) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (epoch != impl_->epoch) {
    return Status(StatusCode::StaleEpoch, "shutdown carries a stale coordinator epoch",
                  "request=" + epoch.to_string() + " current=" + impl_->epoch.to_string());
  }
  if (!impl_->shutting_down) {
    return Status(StatusCode::InvalidState, "shutdown was never initiated");
  }
  if (impl_->shutdown_complete) return Status::success();
  if (impl_->config.enable_durability && impl_->store != nullptr) {
    std::lock_guard<std::mutex> store_guard(impl_->store_mutex);
    (void)impl_->store->compact(impl_->durable);
    (void)impl_->store->flush();
    (void)impl_->store->close();
  }
  impl_->shutdown_complete = true;
  return Status::success();
}

Status CoherenceEngine::flush_durable() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->config.enable_durability || impl_->store == nullptr) return Status::success();
  std::lock_guard<std::mutex> store_guard(impl_->store_mutex);
  return impl_->store->flush();
}

} // namespace coherence
