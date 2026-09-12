// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/enums.hpp"

#include <array>
#include <cstddef>

namespace coherence {
namespace {

template <typename Enum, std::size_t N>
std::string_view lookup_token(const std::pair<Enum, std::string_view> (&table)[N],
                              Enum value) noexcept {
  for (const auto& entry : table) {
    if (entry.first == value) return entry.second;
  }
  return std::string_view{"unknown"};
}

template <typename Enum, std::size_t N>
std::optional<Enum> lookup_value(const std::pair<Enum, std::string_view> (&table)[N],
                                 std::string_view token) noexcept {
  for (const auto& entry : table) {
    if (entry.second == token) return entry.first;
  }
  return std::nullopt;
}

/// Validate a raw byte against the declared members of an enumeration. An
/// undeclared value is rejected; it is never mapped to a member that happens to
/// share a token such as "unknown".
template <typename Enum, std::size_t N>
std::optional<Enum> enum_from_u8_impl(std::uint8_t raw,
                                      const std::pair<Enum, std::string_view> (&table)[N]) noexcept {
  const Enum candidate = static_cast<Enum>(raw);
  for (const auto& entry : table) {
    if (entry.first == candidate) return candidate;
  }
  return std::nullopt;
}

constexpr std::pair<MemoryDomain, std::string_view> kMemoryDomain[] = {
    {MemoryDomain::Unknown, "unknown"},
    {MemoryDomain::HostPageable, "host_pageable"},
    {MemoryDomain::HostPinned, "host_pinned"},
    {MemoryDomain::HostShared, "host_shared"},
    {MemoryDomain::AcceleratorLocal, "accelerator_local"},
    {MemoryDomain::CxlClass, "cxl_class"},
    {MemoryDomain::PersistentMapped, "persistent_mapped"},
    {MemoryDomain::Remote, "remote"},
    {MemoryDomain::Synthetic, "synthetic"},
};

constexpr std::pair<EvidenceClass, std::string_view> kEvidenceClass[] = {
    {EvidenceClass::Unsupported, "unsupported"},
    {EvidenceClass::Synthetic, "synthetic"},
    {EvidenceClass::Real, "real"},
};

constexpr std::pair<CoherenceState, std::string_view> kCoherenceState[] = {
    {CoherenceState::Unknown, "unknown"},
    {CoherenceState::Invalid, "invalid"},
    {CoherenceState::Stale, "stale"},
    {CoherenceState::Current, "current"},
    {CoherenceState::SyncRequired, "sync_required"},
    {CoherenceState::RevalidationRequired, "revalidation_required"},
    {CoherenceState::Dirty, "dirty"},
    {CoherenceState::Fenced, "fenced"},
    {CoherenceState::Retired, "retired"},
};

constexpr std::pair<ObjectLifecycle, std::string_view> kObjectLifecycle[] = {
    {ObjectLifecycle::Created, "created"},
    {ObjectLifecycle::Active, "active"},
    {ObjectLifecycle::Quiescing, "quiescing"},
    {ObjectLifecycle::RecoveryRequired, "recovery_required"},
    {ObjectLifecycle::Retired, "retired"},
};

constexpr std::pair<RegionLifecycle, std::string_view> kRegionLifecycle[] = {
    {RegionLifecycle::Registered, "registered"},
    {RegionLifecycle::Active, "active"},
    {RegionLifecycle::Quiescing, "quiescing"},
    {RegionLifecycle::Retired, "retired"},
};

constexpr std::pair<ParticipantLifecycle, std::string_view> kParticipantLifecycle[] = {
    {ParticipantLifecycle::Observed, "observed"},
    {ParticipantLifecycle::Admitted, "admitted"},
    {ParticipantLifecycle::Active, "active"},
    {ParticipantLifecycle::Degraded, "degraded"},
    {ParticipantLifecycle::Fenced, "fenced"},
    {ParticipantLifecycle::Retired, "retired"},
};

constexpr std::pair<DomainLifecycle, std::string_view> kDomainLifecycle[] = {
    {DomainLifecycle::Created, "created"},
    {DomainLifecycle::Active, "active"},
    {DomainLifecycle::Quiescing, "quiescing"},
    {DomainLifecycle::RecoveryRequired, "recovery_required"},
    {DomainLifecycle::Retired, "retired"},
};

constexpr std::pair<ConsistencyModel, std::string_view> kConsistencyModel[] = {
    {ConsistencyModel::Unspecified, "unspecified"},
    {ConsistencyModel::Strict, "strict"},
    {ConsistencyModel::ReleaseAcquire, "release_acquire"},
    {ConsistencyModel::Snapshot, "snapshot"},
    {ConsistencyModel::Eventual, "eventual"},
};

constexpr std::pair<WriteOwnershipMode, std::string_view> kWriteOwnership[] = {
    {WriteOwnershipMode::Unspecified, "unspecified"},
    {WriteOwnershipMode::SingleWriterExclusive, "single_writer_exclusive"},
    {WriteOwnershipMode::ReadOnlyObject, "read_only_object"},
    {WriteOwnershipMode::MultiWriterUnsupported, "multi_writer_unsupported"},
};

constexpr std::pair<PublicationDurability, std::string_view> kPublicationDurability[] = {
    {PublicationDurability::Unspecified, "unspecified"},
    {PublicationDurability::Ephemeral, "ephemeral"},
    {PublicationDurability::DurableMetadata, "durable_metadata"},
};

constexpr std::pair<ConflictBehavior, std::string_view> kConflictBehavior[] = {
    {ConflictBehavior::Unspecified, "unspecified"},
    {ConflictBehavior::Reject, "reject"},
    {ConflictBehavior::InvalidateReaders, "invalidate_readers"},
};

constexpr std::pair<RecoveryPolicy, std::string_view> kRecoveryPolicy[] = {
    {RecoveryPolicy::Unspecified, "unspecified"},
    {RecoveryPolicy::Conservative, "conservative"},
    {RecoveryPolicy::CompleteDurablePending, "complete_durable_pending"},
};

constexpr std::pair<StaleReadPolicy, std::string_view> kStaleReadPolicy[] = {
    {StaleReadPolicy::Unspecified, "unspecified"},
    {StaleReadPolicy::Never, "never"},
    {StaleReadPolicy::Bounded, "bounded"},
    {StaleReadPolicy::Always, "always"},
};

constexpr std::pair<DirtyLossPolicy, std::string_view> kDirtyLossPolicy[] = {
    {DirtyLossPolicy::Unspecified, "unspecified"},
    {DirtyLossPolicy::ReportUnknown, "report_unknown"},
    {DirtyLossPolicy::ReportLost, "report_lost"},
    {DirtyLossPolicy::RequireRecovery, "require_recovery"},
};

constexpr std::pair<ReadOutcome, std::string_view> kReadOutcome[] = {
    {ReadOutcome::Unknown, "unknown"},
    {ReadOutcome::ReadCurrent, "read_current"},
    {ReadOutcome::ReadAfterSync, "read_after_sync"},
    {ReadOutcome::ReadStaleAllowed, "read_stale_allowed"},
    {ReadOutcome::ReadBlocked, "read_blocked"},
    {ReadOutcome::Unsupported, "unsupported"},
};

constexpr std::pair<AuthorityMode, std::string_view> kAuthorityMode[] = {
    {AuthorityMode::None, "none"},
    {AuthorityMode::SharedReaders, "shared_readers"},
    {AuthorityMode::ExclusiveWriter, "exclusive_writer"},
    {AuthorityMode::TransferPending, "transfer_pending"},
    {AuthorityMode::RevalidationRequired, "revalidation_required"},
};

constexpr std::pair<PublicationState, std::string_view> kPublicationState[] = {
    {PublicationState::None, "none"},
    {PublicationState::PendingDurable, "pending_durable"},
    {PublicationState::Committed, "committed"},
    {PublicationState::Aborted, "aborted"},
    {PublicationState::RecoveryRequired, "recovery_required"},
};

constexpr std::pair<InvalidationState, std::string_view> kInvalidationState[] = {
    {InvalidationState::Requested, "requested"},
    {InvalidationState::Acknowledged, "acknowledged"},
    {InvalidationState::Superseded, "superseded"},
    {InvalidationState::Unknown, "unknown"},
};

constexpr std::pair<DirtyCondition, std::string_view> kDirtyCondition[] = {
    {DirtyCondition::Clean, "clean"},
    {DirtyCondition::DirtyUnpublished, "dirty_unpublished"},
    {DirtyCondition::DirtyPublished, "dirty_published"},
    {DirtyCondition::DirtyLost, "dirty_lost"},
    {DirtyCondition::DirtyUnknown, "dirty_unknown"},
};

constexpr std::pair<SyncOperationKind, std::string_view> kSyncKind[] = {
    {SyncOperationKind::Copy, "copy"},
    {SyncOperationKind::Flush, "flush"},
    {SyncOperationKind::Invalidate, "invalidate"},
    {SyncOperationKind::Reload, "reload"},
    {SyncOperationKind::Publish, "publish"},
    {SyncOperationKind::Acquire, "acquire"},
    {SyncOperationKind::Release, "release"},
    {SyncOperationKind::Revalidate, "revalidate"},
};

constexpr std::pair<SyncState, std::string_view> kSyncState[] = {
    {SyncState::Planned, "planned"},
    {SyncState::Requested, "requested"},
    {SyncState::InProgress, "in_progress"},
    {SyncState::Completed, "completed"},
    {SyncState::Failed, "failed"},
    {SyncState::OutcomeUnknown, "outcome_unknown"},
    {SyncState::Cancelled, "cancelled"},
};

constexpr std::pair<DecisionKind, std::string_view> kDecisionKind[] = {
    {DecisionKind::DomainCreate, "domain_create"},
    {DecisionKind::ParticipantRegister, "participant_register"},
    {DecisionKind::ObjectRegister, "object_register"},
    {DecisionKind::RegionRegister, "region_register"},
    {DecisionKind::ReadAcquire, "read_acquire"},
    {DecisionKind::WriteAcquire, "write_acquire"},
    {DecisionKind::MarkDirty, "mark_dirty"},
    {DecisionKind::Publish, "publish"},
    {DecisionKind::Invalidate, "invalidate"},
    {DecisionKind::SyncBegin, "sync_begin"},
    {DecisionKind::SyncComplete, "sync_complete"},
    {DecisionKind::Release, "release"},
    {DecisionKind::Fence, "fence"},
    {DecisionKind::Recovery, "recovery"},
    {DecisionKind::Audit, "audit"},
    {DecisionKind::PolicyChange, "policy_change"},
    {DecisionKind::RegionRetire, "region_retire"},
    {DecisionKind::ObjectRetire, "object_retire"},
    {DecisionKind::Shutdown, "shutdown"},
    {DecisionKind::Revalidate, "revalidate"},
    {DecisionKind::Recover, "recover"},
};

constexpr std::pair<EvidenceKind, std::string_view> kEvidenceKind[] = {
    {EvidenceKind::None, "none"},
    {EvidenceKind::RuntimeObservation, "runtime_observation"},
    {EvidenceKind::ByteComparison, "byte_comparison"},
    {EvidenceKind::BackendCompletion, "backend_completion"},
    {EvidenceKind::ProcessLifecycle, "process_lifecycle"},
    {EvidenceKind::ExplicitAck, "explicit_ack"},
    {EvidenceKind::PersistedMetadata, "persisted_metadata"},
    {EvidenceKind::HardwareQuery, "hardware_query"},
    {EvidenceKind::SyntheticFixture, "synthetic_fixture"},
};

} // namespace

std::string_view to_token(MemoryDomain value) noexcept { return lookup_token(kMemoryDomain, value); }
std::string_view to_token(EvidenceClass value) noexcept { return lookup_token(kEvidenceClass, value); }
std::string_view to_token(CoherenceState value) noexcept { return lookup_token(kCoherenceState, value); }
std::string_view to_token(ObjectLifecycle value) noexcept { return lookup_token(kObjectLifecycle, value); }
std::string_view to_token(RegionLifecycle value) noexcept { return lookup_token(kRegionLifecycle, value); }
std::string_view to_token(ParticipantLifecycle value) noexcept { return lookup_token(kParticipantLifecycle, value); }
std::string_view to_token(DomainLifecycle value) noexcept { return lookup_token(kDomainLifecycle, value); }
std::string_view to_token(ConsistencyModel value) noexcept { return lookup_token(kConsistencyModel, value); }
std::string_view to_token(WriteOwnershipMode value) noexcept { return lookup_token(kWriteOwnership, value); }
std::string_view to_token(PublicationDurability value) noexcept { return lookup_token(kPublicationDurability, value); }
std::string_view to_token(ConflictBehavior value) noexcept { return lookup_token(kConflictBehavior, value); }
std::string_view to_token(RecoveryPolicy value) noexcept { return lookup_token(kRecoveryPolicy, value); }
std::string_view to_token(StaleReadPolicy value) noexcept { return lookup_token(kStaleReadPolicy, value); }
std::string_view to_token(DirtyLossPolicy value) noexcept { return lookup_token(kDirtyLossPolicy, value); }
std::string_view to_token(ReadOutcome value) noexcept { return lookup_token(kReadOutcome, value); }
std::string_view to_token(AuthorityMode value) noexcept { return lookup_token(kAuthorityMode, value); }
std::string_view to_token(PublicationState value) noexcept { return lookup_token(kPublicationState, value); }
std::string_view to_token(InvalidationState value) noexcept { return lookup_token(kInvalidationState, value); }
std::string_view to_token(DirtyCondition value) noexcept { return lookup_token(kDirtyCondition, value); }
std::string_view to_token(SyncOperationKind value) noexcept { return lookup_token(kSyncKind, value); }
std::string_view to_token(SyncState value) noexcept { return lookup_token(kSyncState, value); }
std::string_view to_token(DecisionKind value) noexcept { return lookup_token(kDecisionKind, value); }
std::string_view to_token(EvidenceKind value) noexcept { return lookup_token(kEvidenceKind, value); }

std::optional<MemoryDomain> parse_memory_domain(std::string_view token) noexcept { return lookup_value(kMemoryDomain, token); }
std::optional<EvidenceClass> parse_evidence_class(std::string_view token) noexcept { return lookup_value(kEvidenceClass, token); }
std::optional<CoherenceState> parse_coherence_state(std::string_view token) noexcept { return lookup_value(kCoherenceState, token); }
std::optional<ObjectLifecycle> parse_object_lifecycle(std::string_view token) noexcept { return lookup_value(kObjectLifecycle, token); }
std::optional<RegionLifecycle> parse_region_lifecycle(std::string_view token) noexcept { return lookup_value(kRegionLifecycle, token); }
std::optional<ParticipantLifecycle> parse_participant_lifecycle(std::string_view token) noexcept { return lookup_value(kParticipantLifecycle, token); }
std::optional<DomainLifecycle> parse_domain_lifecycle(std::string_view token) noexcept { return lookup_value(kDomainLifecycle, token); }
std::optional<ConsistencyModel> parse_consistency_model(std::string_view token) noexcept { return lookup_value(kConsistencyModel, token); }
std::optional<WriteOwnershipMode> parse_write_ownership_mode(std::string_view token) noexcept { return lookup_value(kWriteOwnership, token); }
std::optional<PublicationDurability> parse_publication_durability(std::string_view token) noexcept { return lookup_value(kPublicationDurability, token); }
std::optional<ConflictBehavior> parse_conflict_behavior(std::string_view token) noexcept { return lookup_value(kConflictBehavior, token); }
std::optional<RecoveryPolicy> parse_recovery_policy(std::string_view token) noexcept { return lookup_value(kRecoveryPolicy, token); }
std::optional<StaleReadPolicy> parse_stale_read_policy(std::string_view token) noexcept { return lookup_value(kStaleReadPolicy, token); }
std::optional<DirtyLossPolicy> parse_dirty_loss_policy(std::string_view token) noexcept { return lookup_value(kDirtyLossPolicy, token); }
std::optional<EvidenceKind> parse_evidence_kind(std::string_view token) noexcept { return lookup_value(kEvidenceKind, token); }

std::optional<MemoryDomain> memory_domain_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<MemoryDomain>(raw, kMemoryDomain); }
std::optional<CoherenceState> coherence_state_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<CoherenceState>(raw, kCoherenceState); }
std::optional<EvidenceClass> evidence_class_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<EvidenceClass>(raw, kEvidenceClass); }
std::optional<EvidenceKind> evidence_kind_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<EvidenceKind>(raw, kEvidenceKind); }
std::optional<ObjectLifecycle> object_lifecycle_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<ObjectLifecycle>(raw, kObjectLifecycle); }
std::optional<RegionLifecycle> region_lifecycle_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<RegionLifecycle>(raw, kRegionLifecycle); }
std::optional<ParticipantLifecycle> participant_lifecycle_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<ParticipantLifecycle>(raw, kParticipantLifecycle); }
std::optional<DomainLifecycle> domain_lifecycle_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<DomainLifecycle>(raw, kDomainLifecycle); }
std::optional<SyncState> sync_state_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<SyncState>(raw, kSyncState); }
std::optional<SyncOperationKind> sync_kind_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<SyncOperationKind>(raw, kSyncKind); }
std::optional<PublicationState> publication_state_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<PublicationState>(raw, kPublicationState); }
std::optional<InvalidationState> invalidation_state_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<InvalidationState>(raw, kInvalidationState); }
std::optional<DirtyCondition> dirty_condition_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<DirtyCondition>(raw, kDirtyCondition); }
std::optional<AuthorityMode> authority_mode_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<AuthorityMode>(raw, kAuthorityMode); }
std::optional<DecisionKind> decision_kind_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<DecisionKind>(raw, kDecisionKind); }
std::optional<ReadOutcome> read_outcome_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<ReadOutcome>(raw, kReadOutcome); }
std::optional<ConsistencyModel> consistency_model_from_u8(std::uint8_t raw) noexcept { return enum_from_u8_impl<ConsistencyModel>(raw, kConsistencyModel); }

// ---------------------------------------------------------------------------
// Transition legality.
// ---------------------------------------------------------------------------
bool is_legal_coherence_transition(CoherenceState from, CoherenceState to) noexcept {
  if (from == to) {
    // Idempotent re-observation is legal, except that a terminal state stays
    // terminal and an undetermined state stays undetermined.
    return true;
  }
  switch (from) {
    case CoherenceState::Unknown:
      return to == CoherenceState::Invalid || to == CoherenceState::Stale ||
             to == CoherenceState::Current || to == CoherenceState::SyncRequired ||
             to == CoherenceState::RevalidationRequired || to == CoherenceState::Dirty ||
             to == CoherenceState::Fenced || to == CoherenceState::Retired;
    case CoherenceState::Invalid:
      return to == CoherenceState::Stale || to == CoherenceState::Current ||
             to == CoherenceState::SyncRequired || to == CoherenceState::RevalidationRequired ||
             to == CoherenceState::Fenced || to == CoherenceState::Retired ||
             to == CoherenceState::Unknown;
    case CoherenceState::Stale:
      return to == CoherenceState::Current || to == CoherenceState::Invalid ||
             to == CoherenceState::SyncRequired || to == CoherenceState::RevalidationRequired ||
             to == CoherenceState::Fenced || to == CoherenceState::Retired ||
             to == CoherenceState::Dirty;
    case CoherenceState::Current:
      return to == CoherenceState::Stale || to == CoherenceState::Invalid ||
             to == CoherenceState::Dirty || to == CoherenceState::Fenced ||
             to == CoherenceState::Retired || to == CoherenceState::RevalidationRequired;
    case CoherenceState::SyncRequired:
      return to == CoherenceState::Current || to == CoherenceState::Stale ||
             to == CoherenceState::Invalid || to == CoherenceState::RevalidationRequired ||
             to == CoherenceState::Fenced || to == CoherenceState::Retired;
    case CoherenceState::RevalidationRequired:
      return to == CoherenceState::Current || to == CoherenceState::Stale ||
             to == CoherenceState::Invalid || to == CoherenceState::SyncRequired ||
             to == CoherenceState::Fenced || to == CoherenceState::Retired ||
             to == CoherenceState::Dirty;
    case CoherenceState::Dirty:
      // A dirty replica either becomes current at the new version (after a
      // successful publication), reverts to stale (publication aborted), or is
      // invalidated. It cannot silently become current without a publication.
      return to == CoherenceState::Current || to == CoherenceState::Stale ||
             to == CoherenceState::Invalid || to == CoherenceState::Fenced ||
             to == CoherenceState::Retired;
    case CoherenceState::Fenced:
      return to == CoherenceState::Retired || to == CoherenceState::Invalid ||
             to == CoherenceState::RevalidationRequired;
    case CoherenceState::Retired:
      // Terminal. A retired replica never regains a live state.
      return false;
  }
  return false;
}

bool is_legal_object_transition(ObjectLifecycle from, ObjectLifecycle to) noexcept {
  if (from == to) return true;
  switch (from) {
    case ObjectLifecycle::Created:
      return to == ObjectLifecycle::Active || to == ObjectLifecycle::Retired;
    case ObjectLifecycle::Active:
      return to == ObjectLifecycle::Quiescing || to == ObjectLifecycle::Retired ||
             to == ObjectLifecycle::RecoveryRequired;
    case ObjectLifecycle::Quiescing:
      return to == ObjectLifecycle::Active || to == ObjectLifecycle::Retired ||
             to == ObjectLifecycle::RecoveryRequired;
    case ObjectLifecycle::RecoveryRequired:
      return to == ObjectLifecycle::Active || to == ObjectLifecycle::Retired;
    case ObjectLifecycle::Retired:
      return false;
  }
  return false;
}

bool is_legal_region_transition(RegionLifecycle from, RegionLifecycle to) noexcept {
  if (from == to) return true;
  switch (from) {
    case RegionLifecycle::Registered:
      return to == RegionLifecycle::Active || to == RegionLifecycle::Retired;
    case RegionLifecycle::Active:
      return to == RegionLifecycle::Quiescing || to == RegionLifecycle::Retired;
    case RegionLifecycle::Quiescing:
      return to == RegionLifecycle::Active || to == RegionLifecycle::Retired;
    case RegionLifecycle::Retired:
      return false;
  }
  return false;
}

bool is_legal_participant_transition(ParticipantLifecycle from, ParticipantLifecycle to) noexcept {
  if (from == to) return true;
  switch (from) {
    case ParticipantLifecycle::Observed:
      return to == ParticipantLifecycle::Admitted || to == ParticipantLifecycle::Retired;
    case ParticipantLifecycle::Admitted:
      return to == ParticipantLifecycle::Active || to == ParticipantLifecycle::Fenced ||
             to == ParticipantLifecycle::Retired;
    case ParticipantLifecycle::Active:
      return to == ParticipantLifecycle::Degraded || to == ParticipantLifecycle::Fenced ||
             to == ParticipantLifecycle::Retired;
    case ParticipantLifecycle::Degraded:
      return to == ParticipantLifecycle::Active || to == ParticipantLifecycle::Fenced ||
             to == ParticipantLifecycle::Retired;
    case ParticipantLifecycle::Fenced:
      // A fenced participant is never un-fenced: it must be re-admitted under a
      // fresh ParticipantBootId, which creates a distinct live incarnation.
      return to == ParticipantLifecycle::Retired;
    case ParticipantLifecycle::Retired:
      return false;
  }
  return false;
}

bool is_legal_domain_transition(DomainLifecycle from, DomainLifecycle to) noexcept {
  if (from == to) return true;
  switch (from) {
    case DomainLifecycle::Created:
      return to == DomainLifecycle::Active || to == DomainLifecycle::Retired;
    case DomainLifecycle::Active:
      return to == DomainLifecycle::Quiescing || to == DomainLifecycle::RecoveryRequired ||
             to == DomainLifecycle::Retired;
    case DomainLifecycle::Quiescing:
      return to == DomainLifecycle::Active || to == DomainLifecycle::Retired;
    case DomainLifecycle::RecoveryRequired:
      return to == DomainLifecycle::Active || to == DomainLifecycle::Retired;
    case DomainLifecycle::Retired:
      return false;
  }
  return false;
}

bool is_legal_sync_transition(SyncState from, SyncState to) noexcept {
  if (from == to) return true;
  switch (from) {
    case SyncState::Planned:
      return to == SyncState::Requested || to == SyncState::Cancelled ||
             to == SyncState::Failed;
    case SyncState::Requested:
      return to == SyncState::InProgress || to == SyncState::Completed ||
             to == SyncState::Failed || to == SyncState::Cancelled ||
             to == SyncState::OutcomeUnknown;
    case SyncState::InProgress:
      return to == SyncState::Completed || to == SyncState::Failed ||
             to == SyncState::OutcomeUnknown || to == SyncState::Cancelled;
    case SyncState::Completed:
      // Completion is terminal and idempotent; a duplicate completion must be
      // detected by the caller and must not re-apply a state change.
      return false;
    case SyncState::Failed:
      return to == SyncState::Requested;
    case SyncState::OutcomeUnknown:
      return to == SyncState::Completed || to == SyncState::Failed ||
             to == SyncState::Cancelled;
    case SyncState::Cancelled:
      return false;
  }
  return false;
}

bool is_writable_state(CoherenceState state) noexcept {
  return state == CoherenceState::Current || state == CoherenceState::Dirty;
}

bool is_current_state(CoherenceState state) noexcept { return state == CoherenceState::Current; }

bool is_readable_state(CoherenceState state) noexcept {
  switch (state) {
    case CoherenceState::Current:
    case CoherenceState::Stale:
    case CoherenceState::Dirty:
      return true;
    default:
      return false;
  }
}

bool is_live_state(CoherenceState state) noexcept {
  switch (state) {
    case CoherenceState::Retired:
    case CoherenceState::Fenced:
    case CoherenceState::Invalid:
    case CoherenceState::Unknown:
      return false;
    default:
      return true;
  }
}

bool is_authority_compatible_with_write(CoherenceState state) noexcept {
  return is_live_state(state) && state != CoherenceState::RevalidationRequired;
}

} // namespace coherence
