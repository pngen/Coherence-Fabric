// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Enumerations for the software coherence model, together with the exact
// meaning of every value and the legality of every transition.
//
// The vocabulary deliberately avoids CPU cache-protocol terminology
// (MESI/MOESI) because Coherence Fabric does NOT implement a hardware cache
// protocol. The states below describe software-governed authority and
// visibility over memory representations, not hardware cache line states.
#ifndef COHERENCE_ENUMS_HPP
#define COHERENCE_ENUMS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "coherence/export.hpp"

namespace coherence {

// ---------------------------------------------------------------------------
// Memory domains.
// ---------------------------------------------------------------------------
enum class MemoryDomain : std::uint8_t {
  Unknown = 0,
  HostPageable = 1,
  HostPinned = 2,
  HostShared = 3,
  AcceleratorLocal = 4,
  CxlClass = 5,
  PersistentMapped = 6,
  Remote = 7,
  Synthetic = 8,
};

/// How the contents of a region in this domain are verified by the runtime:
///  - Real        : the runtime can read/compare real bytes it did not invent.
///  - Synthetic   : the domain is a simulation fixture; no physical device.
///  - Unsupported : the runtime has no implementation for the domain.
enum class EvidenceClass : std::uint8_t {
  Unsupported = 0,
  Synthetic = 1,
  Real = 2,
};

// ---------------------------------------------------------------------------
// Replica coherence state.
//
//  Unknown              : no determination possible. Fails closed under strict.
//  Invalid              : contents must not be read; no valid data present.
//  Stale                : contents are known to predate the authoritative
//                         version. Readable only where stale reads are allowed.
//  Current              : contents match the authoritative version AND carry
//                         live evidence of that fact.
//  SyncRequired         : a synchronization must complete before currentness can
//                         be established.
//  RevalidationRequired : dynamic evidence was lost (restart, boot change, epoch
//                         change). Currentness must be re-established with new
//                         evidence; persisted metadata alone is not sufficient.
//  Dirty                : holds an unpublished modification of the authority
//                         holder. Not authoritative for other readers.
//  Fenced               : the owning participant was fenced. Contents are not
//                         usable as a source of truth.
//  Retired              : terminal; the replica no longer participates.
// ---------------------------------------------------------------------------
enum class CoherenceState : std::uint8_t {
  Unknown = 0,
  Invalid = 1,
  Stale = 2,
  Current = 3,
  SyncRequired = 4,
  RevalidationRequired = 5,
  Dirty = 6,
  Fenced = 7,
  Retired = 8,
};

// ---------------------------------------------------------------------------
// Object lifecycle.
// ---------------------------------------------------------------------------
enum class ObjectLifecycle : std::uint8_t {
  Created = 0,
  Active = 1,
  Quiescing = 2,
  RecoveryRequired = 3,
  Retired = 4,
};

// ---------------------------------------------------------------------------
// Region lifecycle (independent of coherence state).
// ---------------------------------------------------------------------------
enum class RegionLifecycle : std::uint8_t {
  Registered = 0,
  Active = 1,
  Quiescing = 2,
  Retired = 3,
};

// ---------------------------------------------------------------------------
// Participant lifecycle.
// ---------------------------------------------------------------------------
enum class ParticipantLifecycle : std::uint8_t {
  Observed = 0,
  Admitted = 1,
  Active = 2,
  Degraded = 3,
  Fenced = 4,
  Retired = 5,
};

// ---------------------------------------------------------------------------
// Domain lifecycle.
// ---------------------------------------------------------------------------
enum class DomainLifecycle : std::uint8_t {
  Created = 0,
  Active = 1,
  Quiescing = 2,
  RecoveryRequired = 3,
  Retired = 4,
};

// ---------------------------------------------------------------------------
// Consistency models. Only models with precise, enforced semantics are
// exposed. Requesting any other value fails closed with Unsupported.
//
// Strict         : readers require live evidence that the replica matches the
//                  authoritative version. A durable publication must commit
//                  before any reader may observe the new version. Missing or
//                  ambiguous evidence => READ_BLOCKED / UNKNOWN. Never guesses.
// ReleaseAcquire : a writer publishes with release ordering: the publication
//                  becomes visible only after every mandatory invalidation has
//                  been acknowledged, and a reader must perform an acquire
//                  synchronization at a version >= the published version before
//                  it may serve a current read from that replica.
// Snapshot       : readers bind to an explicit snapshot VersionId. A replica
//                  whose recorded version is >= the snapshot version and whose
//                  object generation matches may serve the read; the returned
//                  authoritative version is the snapshot version, not the
//                  newest version. Snapshots do not advance.
// Eventual       : readers may serve stale contents. The decision always
//                  reports both the region's version (latest known) and the
//                  authoritative version, so "latest known" is never presented
//                  as "authoritative current". Staleness may be bounded by
//                  policy; exceeding the bound escalates to READ_BLOCKED.
// ---------------------------------------------------------------------------
enum class ConsistencyModel : std::uint8_t {
  Unspecified = 0,
  Strict = 1,
  ReleaseAcquire = 2,
  Snapshot = 3,
  Eventual = 4,
};

// ---------------------------------------------------------------------------
// Write ownership mode.
//
// SingleWriterExclusive : at most one participant may hold write authority for a
//                         given (object, object generation) at a time. The
//                         invariant auditor enforces this.
// ReadOnlyObject        : the object never accepts write authority. Attempts to
//                         acquire write authority are rejected with
//                         NotWriteAuthorized.
// MultiWriterUnsupported: declared multi-writer semantics are NOT implemented.
//                         A policy carrying this value is rejected at policy
//                         validation time with Unsupported so that multi-writer
//                         operation cannot happen by accident.
// ---------------------------------------------------------------------------
enum class WriteOwnershipMode : std::uint8_t {
  Unspecified = 0,
  SingleWriterExclusive = 1,
  ReadOnlyObject = 2,
  MultiWriterUnsupported = 3,
};

// ---------------------------------------------------------------------------
// Publication durability. Defines the exact point at which a publication
// becomes authoritative.
// ---------------------------------------------------------------------------
enum class PublicationDurability : std::uint8_t {
  Unspecified = 0,
  /// Become authoritative as soon as the coordinator commits in memory. There
  /// is no durable record; a coordinator restart loses the version. Documented
  /// as intentional and reported as ephemeral by the CLI.
  Ephemeral = 1,
  /// Commit durably before any reader may observe the new version. The
  /// durability point is the successful atomic replacement of the metadata
  /// file containing the pending publication record, and the subsequent
  /// in-memory commit of that same PublicationId.
  DurableMetadata = 2,
};

// ---------------------------------------------------------------------------
// Behaviour when a write is requested while incompatible read/write authority
// exists elsewhere.
// ---------------------------------------------------------------------------
enum class ConflictBehavior : std::uint8_t {
  Unspecified = 0,
  /// Reject the new writer with WriteConflict. Existing authority is preserved.
  Reject = 1,
  /// Invalidate incompatible readers and grant the writer once every mandatory
  /// invalidation has been acknowledged. If an acknowledgement cannot be
  /// obtained the write is not granted.
  InvalidateReaders = 2,
};

// ---------------------------------------------------------------------------
// Recovery policy applied by a coordinator after restart.
// ---------------------------------------------------------------------------
enum class RecoveryPolicy : std::uint8_t {
  Unspecified = 0,
  /// Do not trust any dynamic state. Every region becomes
  /// RevalidationRequired; authority from before the restart is fenced.
  Conservative = 1,
  /// Conservative, plus: a pending publication whose durable record was
  /// committed before the crash is completed under the SAME PublicationId and
  /// VersionId. No new or second version is invented.
  CompleteDurablePending = 2,
};

// ---------------------------------------------------------------------------
// What a reader is allowed to do with contents that are not current.
// ---------------------------------------------------------------------------
enum class StaleReadPolicy : std::uint8_t {
  Unspecified = 0,
  Never = 1,
  /// Allowed while (authoritative version - replica version) <= bound.
  Bounded = 2,
  Always = 3,
};

// ---------------------------------------------------------------------------
// What the runtime reports when the only dirty authority holder disappears
// before publication.
// ---------------------------------------------------------------------------
enum class DirtyLossPolicy : std::uint8_t {
  Unspecified = 0,
  /// Report DirtyUnpublished + OutcomeUnknown.
  ReportUnknown = 1,
  /// Report DirtyLost: the newest state is known to be gone.
  ReportLost = 2,
  /// Require explicit operator resolution before the object can be used again.
  RequireRecovery = 3,
};

// ---------------------------------------------------------------------------
// Read decision outcome.
// ---------------------------------------------------------------------------
enum class ReadOutcome : std::uint8_t {
  Unknown = 0,
  /// The selected replica is current and carries live evidence.
  ReadCurrent = 1,
  /// The caller must first complete the identified synchronization; the
  /// decision reports the required operation and does not authorize a read.
  ReadAfterSync = 2,
  /// Stale contents may be used under policy. The decision reports the region
  /// version (latest known) and the authoritative version separately.
  ReadStaleAllowed = 3,
  /// The read must not proceed.
  ReadBlocked = 4,
  /// Policy or backend cannot answer the question at all.
  Unsupported = 5,
};

// ---------------------------------------------------------------------------
// Authority currently held for an object.
// ---------------------------------------------------------------------------
enum class AuthorityMode : std::uint8_t {
  None = 0,
  SharedReaders = 1,
  ExclusiveWriter = 2,
  /// Write authority granted, invalidations still outstanding. The writer may
  /// not mutate yet.
  TransferPending = 3,
  /// Dynamic authority was lost (restart, fence). Explicit revalidation is
  /// required before authority can be re-acquired.
  RevalidationRequired = 4,
};

// ---------------------------------------------------------------------------
// Publication state.
// ---------------------------------------------------------------------------
enum class PublicationState : std::uint8_t {
  None = 0,
  /// Durable pending record written; not yet visible to readers.
  PendingDurable = 1,
  Committed = 2,
  Aborted = 3,
  /// A durable pending record exists whose outcome the coordinator cannot
  /// determine from the evidence it holds.
  RecoveryRequired = 4,
};

// ---------------------------------------------------------------------------
// Invalidation state.
// ---------------------------------------------------------------------------
enum class InvalidationState : std::uint8_t {
  Requested = 0,
  Acknowledged = 1,
  /// The targeted replica generation no longer exists; the invalidation was
  /// satisfied vacuously and must not be applied to a later generation.
  Superseded = 2,
  Unknown = 3,
};

// ---------------------------------------------------------------------------
// Dirty tracking state for an object/region.
// ---------------------------------------------------------------------------
enum class DirtyCondition : std::uint8_t {
  Clean = 0,
  /// Holds an unpublished modification; the authority holder is alive.
  DirtyUnpublished = 1,
  /// The modification was published; the version is authoritative.
  DirtyPublished = 2,
  /// The only holder of the unpublished modification disappeared. The newest
  /// state is known to be gone.
  DirtyLost = 3,
  /// The runtime cannot determine whether the modification survived.
  DirtyUnknown = 4,
};

// ---------------------------------------------------------------------------
// Synchronization operation kind.
// ---------------------------------------------------------------------------
enum class SyncOperationKind : std::uint8_t {
  Copy = 0,
  Flush = 1,
  Invalidate = 2,
  Reload = 3,
  Publish = 4,
  Acquire = 5,
  Release = 6,
  Revalidate = 7,
};

enum class SyncState : std::uint8_t {
  Planned = 0,
  Requested = 1,
  InProgress = 2,
  Completed = 3,
  Failed = 4,
  /// The operation was dispatched but its completion could not be established.
  OutcomeUnknown = 5,
  Cancelled = 6,
};

// ---------------------------------------------------------------------------
// Decision kinds recorded in the deterministic decision log.
// ---------------------------------------------------------------------------
enum class DecisionKind : std::uint8_t {
  DomainCreate = 0,
  ParticipantRegister = 1,
  ObjectRegister = 2,
  RegionRegister = 3,
  ReadAcquire = 4,
  WriteAcquire = 5,
  MarkDirty = 6,
  Publish = 7,
  Invalidate = 8,
  SyncBegin = 9,
  SyncComplete = 10,
  Release = 11,
  Fence = 12,
  Recovery = 13,
  Audit = 14,
  PolicyChange = 15,
  RegionRetire = 16,
  ObjectRetire = 17,
  Shutdown = 18,
  Revalidate = 19,
  Recover = 20,
};

// ---------------------------------------------------------------------------
// Evidence provenance.
// ---------------------------------------------------------------------------
enum class EvidenceKind : std::uint8_t {
  None = 0,
  /// Direct observation by the runtime inside the current coordinator epoch.
  RuntimeObservation = 1,
  /// Byte-for-byte comparison of real contents.
  ByteComparison = 2,
  /// A backend (device, transport, mapping) reported completion.
  BackendCompletion = 3,
  /// A process lifecycle event (admission, restart, exit).
  ProcessLifecycle = 4,
  /// An explicit acknowledgement from the affected participant.
  ExplicitAck = 5,
  /// Metadata restored from durable storage. Never sufficient on its own to
  /// re-establish dynamic currentness.
  PersistedMetadata = 6,
  /// A hardware query (device enumeration, capability probe).
  HardwareQuery = 7,
  /// A fixture supplied by a synthetic backend.
  SyntheticFixture = 8,
};

// ---------------------------------------------------------------------------
// Name tables. Stable tokens; part of the public contract.
// ---------------------------------------------------------------------------
COHERENCE_API std::string_view to_token(MemoryDomain value) noexcept;
COHERENCE_API std::string_view to_token(EvidenceClass value) noexcept;
COHERENCE_API std::string_view to_token(CoherenceState value) noexcept;
COHERENCE_API std::string_view to_token(ObjectLifecycle value) noexcept;
COHERENCE_API std::string_view to_token(RegionLifecycle value) noexcept;
COHERENCE_API std::string_view to_token(ParticipantLifecycle value) noexcept;
COHERENCE_API std::string_view to_token(DomainLifecycle value) noexcept;
COHERENCE_API std::string_view to_token(ConsistencyModel value) noexcept;
COHERENCE_API std::string_view to_token(WriteOwnershipMode value) noexcept;
COHERENCE_API std::string_view to_token(PublicationDurability value) noexcept;
COHERENCE_API std::string_view to_token(ConflictBehavior value) noexcept;
COHERENCE_API std::string_view to_token(RecoveryPolicy value) noexcept;
COHERENCE_API std::string_view to_token(StaleReadPolicy value) noexcept;
COHERENCE_API std::string_view to_token(DirtyLossPolicy value) noexcept;
COHERENCE_API std::string_view to_token(ReadOutcome value) noexcept;
COHERENCE_API std::string_view to_token(AuthorityMode value) noexcept;
COHERENCE_API std::string_view to_token(PublicationState value) noexcept;
COHERENCE_API std::string_view to_token(InvalidationState value) noexcept;
COHERENCE_API std::string_view to_token(DirtyCondition value) noexcept;
COHERENCE_API std::string_view to_token(SyncOperationKind value) noexcept;
COHERENCE_API std::string_view to_token(SyncState value) noexcept;
COHERENCE_API std::string_view to_token(DecisionKind value) noexcept;
COHERENCE_API std::string_view to_token(EvidenceKind value) noexcept;

// Validating parsers. An unknown token is rejected (std::nullopt) so that
// persisted or peer-supplied enum values can never be silently mapped to a
// default that would grant authority.
COHERENCE_API std::optional<MemoryDomain> parse_memory_domain(std::string_view token) noexcept;
COHERENCE_API std::optional<EvidenceClass> parse_evidence_class(std::string_view token) noexcept;
COHERENCE_API std::optional<CoherenceState> parse_coherence_state(std::string_view token) noexcept;
COHERENCE_API std::optional<ObjectLifecycle> parse_object_lifecycle(std::string_view token) noexcept;
COHERENCE_API std::optional<RegionLifecycle> parse_region_lifecycle(std::string_view token) noexcept;
COHERENCE_API std::optional<ParticipantLifecycle> parse_participant_lifecycle(std::string_view token) noexcept;
COHERENCE_API std::optional<DomainLifecycle> parse_domain_lifecycle(std::string_view token) noexcept;
COHERENCE_API std::optional<ConsistencyModel> parse_consistency_model(std::string_view token) noexcept;
COHERENCE_API std::optional<WriteOwnershipMode> parse_write_ownership_mode(std::string_view token) noexcept;
COHERENCE_API std::optional<PublicationDurability> parse_publication_durability(std::string_view token) noexcept;
COHERENCE_API std::optional<ConflictBehavior> parse_conflict_behavior(std::string_view token) noexcept;
COHERENCE_API std::optional<RecoveryPolicy> parse_recovery_policy(std::string_view token) noexcept;
COHERENCE_API std::optional<StaleReadPolicy> parse_stale_read_policy(std::string_view token) noexcept;
COHERENCE_API std::optional<DirtyLossPolicy> parse_dirty_loss_policy(std::string_view token) noexcept;
COHERENCE_API std::optional<EvidenceKind> parse_evidence_kind(std::string_view token) noexcept;

/// Numeric validating decoders used by the binary codecs: an out-of-range byte
/// is rejected rather than truncated.
COHERENCE_API std::optional<MemoryDomain> memory_domain_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<CoherenceState> coherence_state_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<EvidenceClass> evidence_class_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<EvidenceKind> evidence_kind_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<ObjectLifecycle> object_lifecycle_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<RegionLifecycle> region_lifecycle_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<ParticipantLifecycle> participant_lifecycle_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<DomainLifecycle> domain_lifecycle_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<SyncState> sync_state_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<SyncOperationKind> sync_kind_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<PublicationState> publication_state_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<InvalidationState> invalidation_state_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<DirtyCondition> dirty_condition_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<AuthorityMode> authority_mode_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<DecisionKind> decision_kind_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<ReadOutcome> read_outcome_from_u8(std::uint8_t raw) noexcept;
COHERENCE_API std::optional<ConsistencyModel> consistency_model_from_u8(std::uint8_t raw) noexcept;

// ---------------------------------------------------------------------------
// Transition legality. Impossible combinations are rejected, not repaired.
// ---------------------------------------------------------------------------

/// Legal coherence-state transitions for a single replica. Self transitions are
/// legal only where they are meaningful (idempotent operations).
COHERENCE_API bool is_legal_coherence_transition(CoherenceState from, CoherenceState to) noexcept;

/// Legal object lifecycle transitions.
COHERENCE_API bool is_legal_object_transition(ObjectLifecycle from, ObjectLifecycle to) noexcept;

/// Legal region lifecycle transitions.
COHERENCE_API bool is_legal_region_transition(RegionLifecycle from, RegionLifecycle to) noexcept;

/// Legal participant lifecycle transitions.
COHERENCE_API bool is_legal_participant_transition(ParticipantLifecycle from,
                                                   ParticipantLifecycle to) noexcept;

/// Legal domain lifecycle transitions.
COHERENCE_API bool is_legal_domain_transition(DomainLifecycle from, DomainLifecycle to) noexcept;

/// Legal synchronization state transitions.
COHERENCE_API bool is_legal_sync_transition(SyncState from, SyncState to) noexcept;

/// True when a replica in this state may be written by the authority holder.
COHERENCE_API bool is_writable_state(CoherenceState state) noexcept;

/// True when a replica in this state may satisfy a read that demands current
/// contents under strict policy. Only Current qualifies.
COHERENCE_API bool is_current_state(CoherenceState state) noexcept;

/// True when a replica in this state may supply contents at all (as latest
/// known or as stale), regardless of policy.
COHERENCE_API bool is_readable_state(CoherenceState state) noexcept;

/// True when the replica still participates in the coherence domain.
COHERENCE_API bool is_live_state(CoherenceState state) noexcept;

/// True when the state is compatible with the participant holding live
/// authority for the object.
COHERENCE_API bool is_authority_compatible_with_write(CoherenceState state) noexcept;

} // namespace coherence

#endif // COHERENCE_ENUMS_HPP
