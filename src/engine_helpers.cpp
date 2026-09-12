// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "engine_helpers.hpp"

#include <algorithm>
#include <string>

namespace coherence {
namespace engine_detail {

Status resolve_locked(CoherenceEngine::Impl& state, const AuthorityContext& ctx, Resolved& out) {
  if (state.shutting_down) {
    return Status(StatusCode::ShuttingDown, "the coherence engine is shutting down");
  }
  if (ctx.participant.is_nil()) {
    return Status(StatusCode::InvalidArgument,
                  "a participant-bound operation requires a participant identity");
  }
  if (ctx.epoch != state.epoch) {
    return Status(StatusCode::StaleEpoch, "request carries a stale coordinator epoch",
                  "request=" + ctx.epoch.to_string() + " current=" + state.epoch.to_string());
  }
  const auto object_it = state.objects.find(ctx.object);
  if (object_it == state.objects.end()) {
    return Status(StatusCode::UnknownObject, "no such coherence object", ctx.object.to_string());
  }
  out.object = &object_it->second;
  if (out.object->generation != ctx.object_generation) {
    return Status(StatusCode::StaleObjectGeneration,
                  "request references a stale object generation",
                  "request=" + ctx.object_generation.to_string() +
                      " current=" + out.object->generation.to_string());
  }
  if (out.object->lifecycle == ObjectLifecycle::Retired) {
    return Status(StatusCode::Retired, "the coherence object has been retired", out.object->name);
  }
  if (out.object->lifecycle == ObjectLifecycle::RecoveryRequired) {
    return Status(StatusCode::RecoveryRequired,
                  "the coherence object requires explicit recovery before further use",
                  out.object->recovery_note);
  }
  if (out.object->lifecycle == ObjectLifecycle::Quiescing) {
    return Status(StatusCode::Quiescing, "the coherence object is quiescing", out.object->name);
  }
  if (ctx.policy_generation.defined() && ctx.policy_generation != out.object->policy_generation) {
    return Status(StatusCode::StalePolicy, "request carries a stale policy generation",
                  "request=" + ctx.policy_generation.to_string() +
                      " current=" + out.object->policy_generation.to_string());
  }
  const auto participant_it = state.participants.find(ctx.participant);
  if (participant_it == state.participants.end()) {
    return Status(StatusCode::UnknownParticipant, "no such participant",
                  ctx.participant.to_string());
  }
  out.participant = &participant_it->second;
  if (out.participant->lifecycle == ParticipantLifecycle::Fenced) {
    return Status(StatusCode::Fenced, "the participant has been fenced",
                  out.participant->fence_reason);
  }
  if (out.participant->lifecycle == ParticipantLifecycle::Retired) {
    return Status(StatusCode::Retired, "the participant has been retired", out.participant->name);
  }
  if (out.participant->boot != ctx.boot) {
    return Status(StatusCode::StaleBoot, "request carries a boot identity that is not live",
                  "request=" + ctx.boot.to_string() + " live=" + out.participant->boot.to_string());
  }
  return Status::success();
}

Status resolve_internal_locked(CoherenceEngine::Impl& state, const AuthorityContext& ctx,
                               ObjectRecord*& object) {
  if (state.shutting_down) {
    return Status(StatusCode::ShuttingDown, "the coherence engine is shutting down");
  }
  if (ctx.epoch != state.epoch) {
    return Status(StatusCode::StaleEpoch, "request carries a stale coordinator epoch",
                  "request=" + ctx.epoch.to_string() + " current=" + state.epoch.to_string());
  }
  const auto it = state.objects.find(ctx.object);
  if (it == state.objects.end()) {
    return Status(StatusCode::UnknownObject, "no such coherence object", ctx.object.to_string());
  }
  if (it->second.generation != ctx.object_generation) {
    return Status(StatusCode::StaleObjectGeneration, "request references a stale object generation",
                  "request=" + ctx.object_generation.to_string() +
                      " current=" + it->second.generation.to_string());
  }
  if (it->second.lifecycle == ObjectLifecycle::Retired) {
    return Status(StatusCode::Retired, "the coherence object has been retired", it->second.name);
  }
  object = &it->second;
  return Status::success();
}

Status resolve_region_locked(CoherenceEngine::Impl& state, const Resolved& resolved, RegionId id,
                             RegionGeneration generation, RegionRecord*& out) {
  const auto it = state.regions.find(id);
  if (it == state.regions.end()) {
    return Status(StatusCode::UnknownRegion, "no such region", id.to_string());
  }
  if (it->second.object != resolved.object->id) {
    return Status(StatusCode::InvalidArgument, "the region does not belong to the object",
                  id.to_string());
  }
  if (generation.defined() && it->second.generation != generation) {
    return Status(StatusCode::StaleRegionGeneration,
                  "request references a stale region generation",
                  "request=" + generation.to_string() +
                      " current=" + it->second.generation.to_string());
  }
  out = &it->second;
  return Status::success();
}

const CoherencePolicy* policy_locked(CoherenceEngine::Impl& state, const ObjectRecord& object) {
  const auto it = state.policies.find(object.policy);
  return it == state.policies.end() ? nullptr : &it->second;
}

bool is_synthetic_domain(MemoryDomain domain) noexcept {
  // A synthetic CXL-class domain has no physical hardware behind it, so
  // fixture evidence is the strongest evidence it can produce.
  return domain == MemoryDomain::Synthetic || domain == MemoryDomain::CxlClass;
}

FreshnessAssessment assess_region_locked(CoherenceEngine::Impl& state, const CoherencePolicy& policy,
                                         const RegionRecord& region) {
  EvidenceContext context;
  context.current_epoch = state.epoch;
  context.participant = region.participant;
  const auto participant = state.participants.find(region.participant);
  context.live_boot =
      participant != state.participants.end() ? participant->second.boot : ParticipantBootId::nil();
  context.current_sequence = state.sequence;
  context.max_age_operations = policy.evidence_max_age_operations;
  context.synthetic_domain = is_synthetic_domain(region.memory_domain);

  EvidenceRecord record;
  if (region.evidence.defined()) {
    record.id = region.evidence;
    record.generation = region.evidence_generation;
    record.kind = EvidenceKind::ByteComparison;
    record.evidence_class = region.evidence_class;
    record.participant = region.participant;
    record.boot = region.boot;
    record.epoch = state.epoch;
    record.region = region.id;
    record.region_generation = region.generation;
    record.observed_version = region.version;
    record.observed_digest = region.content.digest;
    record.observed_crc32c = region.content.crc32c;
    record.sequence = region.updated_sequence;
    record.process_local = true;
  } else {
    record.id = EvidenceId::nil();
    record.kind = EvidenceKind::None;
  }
  return assess_evidence(record, context);
}

RegionId select_read_region_locked(CoherenceEngine::Impl& state, ObjectRecord& object,
                                   RegionId requested) {
  if (requested.defined()) return requested;
  // Deterministic selection: lowest region identity that is current, else the
  // lowest region identity that holds any contents at all. Never dependent on
  // map iteration order or on insertion history.
  //
  // The previously selected replica is re-validated first. This is an
  // accelerator only: if it is no longer current the selection falls back to a
  // full scan, so a stale cache costs time and never changes the answer.
  if (object.preferred_current_replica.defined()) {
    const auto cached = state.regions.find(object.preferred_current_replica);
    if (cached != state.regions.end() && is_current_state(cached->second.state)) {
      return object.preferred_current_replica;
    }
    object.preferred_current_replica = RegionId::nil();
  }
  RegionId best_current;
  RegionId best_readable;
  for (const RegionId id : object.replicas) {
    const auto it = state.regions.find(id);
    if (it == state.regions.end()) continue;
    const CoherenceState st = it->second.state;
    if (is_current_state(st) && !best_current.defined()) best_current = id;
    if (is_readable_state(st) && !best_readable.defined()) best_readable = id;
  }
  if (best_current.defined()) {
    object.preferred_current_replica = best_current;
    return best_current;
  }
  return best_readable;
}

void add_read_grant_locked(CoherenceEngine::Impl& state, ObjectRecord& object,
                           const RegionRecord& region, ParticipantId participant,
                           ParticipantBootId boot, LeaseId lease) {
  for (ReadGrant& grant : object.reads) {
    if (grant.released) continue;
    if (grant.participant == participant && grant.boot == boot && grant.region == region.id &&
        grant.version == region.version) {
      return;  // an identical live grant already exists
    }
  }
  // While an exclusive writer holds authority there is nobody left to protect
  // with a read lease: the next write transfer invalidates every live replica
  // regardless. Issuing one anyway would assert that read authority coexists
  // with exclusive write authority, which single-writer policy forbids.
  if (object.authority == AuthorityMode::ExclusiveWriter ||
      object.authority == AuthorityMode::TransferPending) {
    return;
  }
  if (object.reads.size() >= state.config.max_reads_per_object) return;
  ReadGrant grant;
  grant.lease = lease;
  grant.participant = participant;
  grant.boot = boot;
  grant.object = object.id;
  grant.object_generation = object.generation;
  grant.ownership_generation = object.ownership_generation;
  grant.version = region.version;
  grant.region = region.id;
  grant.region_generation = region.generation;
  grant.granted_sequence = state.sequence;
  object.reads.push_back(grant);
  if (object.authority == AuthorityMode::None) object.authority = AuthorityMode::SharedReaders;
}

void push_region_entry(std::vector<JournalEntry>& entries, const RegionRecord& region) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertRegion;
  entry.region = region;
  entries.push_back(std::move(entry));
}

void push_object_entry(std::vector<JournalEntry>& entries, const ObjectRecord& object) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertObject;
  entry.object = object;
  scrub_object_for_persistence(entry.object);
  entries.push_back(std::move(entry));
}

void push_invalidation_entry(std::vector<JournalEntry>& entries,
                             const InvalidationRecord& record) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertInvalidation;
  entry.invalidation = record;
  entries.push_back(std::move(entry));
}

void push_sync_entry(std::vector<JournalEntry>& entries, const SyncRecord& record) {
  JournalEntry entry;
  entry.kind = JournalEntryKind::UpsertSync;
  entry.sync = record;
  entries.push_back(std::move(entry));
}

} // namespace engine_detail
} // namespace coherence
