// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
// Internal engine helpers shared across the operation translation units.
#ifndef COHERENCE_SRC_ENGINE_HELPERS_HPP
#define COHERENCE_SRC_ENGINE_HELPERS_HPP

#include <vector>

#include "coherence/decision.hpp"
#include "coherence/engine.hpp"
#include "coherence/evidence.hpp"
#include "engine_impl.hpp"

namespace coherence {
namespace engine_detail {

struct Resolved {
  ObjectRecord* object = nullptr;
  RegionRecord* region = nullptr;
  ParticipantRecord* participant = nullptr;
};

/// Full participant-bound authority validation: coordinator epoch, object
/// generation, object lifecycle, policy generation, participant lifecycle and
/// participant boot identity. Every failure maps to a distinct status code.
Status resolve_locked(CoherenceEngine::Impl& state, const AuthorityContext& ctx, Resolved& out);

/// Coordinator-internal validation for operations the coordinator performs on
/// its own behalf (invalidation). Such a request carries a nil participant
/// identity but is still bound to the epoch, object generation and ownership
/// generation.
Status resolve_internal_locked(CoherenceEngine::Impl& state, const AuthorityContext& ctx,
                               ObjectRecord*& object);

Status resolve_region_locked(CoherenceEngine::Impl& state, const Resolved& resolved, RegionId id,
                             RegionGeneration generation, RegionRecord*& out);

const CoherencePolicy* policy_locked(CoherenceEngine::Impl& state, const ObjectRecord& object);

bool is_synthetic_domain(MemoryDomain domain) noexcept;

FreshnessAssessment assess_region_locked(CoherenceEngine::Impl& state, const CoherencePolicy& policy,
                                         const RegionRecord& region);

RegionId select_read_region_locked(CoherenceEngine::Impl& state, ObjectRecord& object,
                                   RegionId requested);

void add_read_grant_locked(CoherenceEngine::Impl& state, ObjectRecord& object,
                           const RegionRecord& region, ParticipantId participant,
                           ParticipantBootId boot, LeaseId lease);

void push_region_entry(std::vector<JournalEntry>& entries, const RegionRecord& region);
void push_object_entry(std::vector<JournalEntry>& entries, const ObjectRecord& object);
void push_invalidation_entry(std::vector<JournalEntry>& entries, const InvalidationRecord& record);
void push_sync_entry(std::vector<JournalEntry>& entries, const SyncRecord& record);

} // namespace engine_detail
} // namespace coherence

#endif // COHERENCE_SRC_ENGINE_HELPERS_HPP
