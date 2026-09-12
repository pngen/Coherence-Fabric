// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/status.hpp"

namespace coherence {
namespace {

struct CodeName {
  StatusCode code;
  std::string_view name;
  StatusSeverity severity;
};

// The table is the single source of truth for the stable status tokens used by
// tests, the CLI, protocol responses and documentation.
constexpr CodeName kCodes[] = {
    {StatusCode::Ok, "ok", StatusSeverity::Success},

    {StatusCode::InvalidArgument, "invalid_argument", StatusSeverity::Permanent},
    {StatusCode::InvalidState, "invalid_state", StatusSeverity::Conflict},
    {StatusCode::InvalidTransition, "invalid_transition", StatusSeverity::Conflict},
    {StatusCode::Unsupported, "unsupported", StatusSeverity::Permanent},
    {StatusCode::CapacityExceeded, "capacity_exceeded", StatusSeverity::Permanent},
    {StatusCode::ResourceExhausted, "resource_exhausted", StatusSeverity::Transient},
    {StatusCode::DuplicateIdentity, "duplicate_identity", StatusSeverity::Conflict},

    {StatusCode::UnknownDomain, "unknown_domain", StatusSeverity::Permanent},
    {StatusCode::UnknownObject, "unknown_object", StatusSeverity::Permanent},
    {StatusCode::UnknownRegion, "unknown_region", StatusSeverity::Permanent},
    {StatusCode::UnknownParticipant, "unknown_participant", StatusSeverity::Permanent},
    {StatusCode::UnknownPolicy, "unknown_policy", StatusSeverity::Permanent},
    {StatusCode::UnknownPublication, "unknown_publication", StatusSeverity::Permanent},
    {StatusCode::UnknownInvalidation, "unknown_invalidation", StatusSeverity::Permanent},
    {StatusCode::UnknownSyncOperation, "unknown_sync_operation", StatusSeverity::Permanent},
    {StatusCode::UnknownRequest, "unknown_request", StatusSeverity::Permanent},

    {StatusCode::StaleEpoch, "stale_epoch", StatusSeverity::Transient},
    {StatusCode::StaleBoot, "stale_boot", StatusSeverity::Transient},
    {StatusCode::StaleObjectGeneration, "stale_object_generation", StatusSeverity::Transient},
    {StatusCode::StaleRegionGeneration, "stale_region_generation", StatusSeverity::Transient},
    {StatusCode::StaleReplicaGeneration, "stale_replica_generation", StatusSeverity::Transient},
    {StatusCode::StaleOwnership, "stale_ownership", StatusSeverity::Transient},
    {StatusCode::StalePolicy, "stale_policy", StatusSeverity::Transient},
    {StatusCode::StalePublication, "stale_publication", StatusSeverity::Transient},
    {StatusCode::StaleInvalidation, "stale_invalidation", StatusSeverity::Transient},
    {StatusCode::StaleSyncOperation, "stale_sync_operation", StatusSeverity::Transient},
    {StatusCode::StaleSession, "stale_session", StatusSeverity::Transient},
    {StatusCode::ReplayedRequest, "replayed_request", StatusSeverity::Conflict},

    {StatusCode::NotAuthoritative, "not_authoritative", StatusSeverity::Conflict},
    {StatusCode::ReadNotCurrent, "read_not_current", StatusSeverity::Conflict},
    {StatusCode::WriteConflict, "write_conflict", StatusSeverity::Conflict},
    {StatusCode::ExclusiveWriterConflict, "exclusive_writer_conflict", StatusSeverity::Conflict},
    {StatusCode::NotWriteAuthorized, "not_write_authorized", StatusSeverity::Conflict},
    {StatusCode::Fenced, "fenced", StatusSeverity::Conflict},
    {StatusCode::Retired, "retired", StatusSeverity::Permanent},

    {StatusCode::SyncRequired, "sync_required", StatusSeverity::Conflict},
    {StatusCode::RevalidationRequired, "revalidation_required", StatusSeverity::Conflict},
    {StatusCode::InvalidationOutstanding, "invalidation_outstanding", StatusSeverity::Conflict},
    {StatusCode::EvidenceMissing, "evidence_missing", StatusSeverity::Conflict},
    {StatusCode::EvidenceStale, "evidence_stale", StatusSeverity::Conflict},
    {StatusCode::DirtyUnpublished, "dirty_unpublished", StatusSeverity::Conflict},
    {StatusCode::DirtyLost, "dirty_lost", StatusSeverity::Conflict},
    {StatusCode::RecoveryRequired, "recovery_required", StatusSeverity::Conflict},
    {StatusCode::Quiescing, "quiescing", StatusSeverity::Transient},
    {StatusCode::ShuttingDown, "shutting_down", StatusSeverity::Transient},

    {StatusCode::OutcomeUnknown, "outcome_unknown", StatusSeverity::Transient},
    {StatusCode::ContentMismatch, "content_mismatch", StatusSeverity::Conflict},

    {StatusCode::IntegrityFailure, "integrity_failure", StatusSeverity::Integrity},
    {StatusCode::CorruptionDetected, "corruption_detected", StatusSeverity::Integrity},
    {StatusCode::TruncatedInput, "truncated_input", StatusSeverity::Integrity},
    {StatusCode::TrailingGarbage, "trailing_garbage", StatusSeverity::Integrity},
    {StatusCode::OversizedInput, "oversized_input", StatusSeverity::Permanent},
    {StatusCode::ProtocolViolation, "protocol_violation", StatusSeverity::Integrity},
    {StatusCode::UnsupportedSchema, "unsupported_schema", StatusSeverity::Permanent},
    {StatusCode::PersistenceFailure, "persistence_failure", StatusSeverity::Integrity},
    {StatusCode::TransportFailure, "transport_failure", StatusSeverity::Transient},
    {StatusCode::ConnectionClosed, "connection_closed", StatusSeverity::Transient},
    {StatusCode::Timeout, "timeout", StatusSeverity::Transient},

    {StatusCode::InternalInvariantViolation, "internal_invariant_violation", StatusSeverity::Internal},
    {StatusCode::InternalError, "internal_error", StatusSeverity::Internal},
};

constexpr const CodeName* find(StatusCode code) noexcept {
  for (const CodeName& entry : kCodes) {
    if (entry.code == code) return &entry;
  }
  return nullptr;
}

} // namespace

std::string_view status_code_name(StatusCode code) noexcept {
  const CodeName* entry = find(code);
  return entry != nullptr ? entry->name : std::string_view{"unknown_status"};
}

bool is_stale_code(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::StaleEpoch:
    case StatusCode::StaleBoot:
    case StatusCode::StaleObjectGeneration:
    case StatusCode::StaleRegionGeneration:
    case StatusCode::StaleReplicaGeneration:
    case StatusCode::StaleOwnership:
    case StatusCode::StalePolicy:
    case StatusCode::StalePublication:
    case StatusCode::StaleInvalidation:
    case StatusCode::StaleSyncOperation:
    case StatusCode::StaleSession:
      return true;
    default:
      return false;
  }
}

StatusSeverity status_severity(StatusCode code) noexcept {
  const CodeName* entry = find(code);
  return entry != nullptr ? entry->severity : StatusSeverity::Internal;
}

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status::Status(StatusCode code, std::string message, std::string detail)
    : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

std::string Status::to_string() const {
  std::string out(status_code_name(code_));
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  if (!detail_.empty()) {
    out.append(" (");
    out.append(detail_);
    out.push_back(')');
  }
  return out;
}

} // namespace coherence
