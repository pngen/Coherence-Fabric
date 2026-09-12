// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Evidence model.
//
// No currentness claim exists without provenance. Every decision that asserts a
// replica is current, that a publication committed, or that an invalidation was
// observed must be supported by an EvidenceRecord that survives scrutiny.
//
// Two rules are enforced by the runtime rather than merely documented:
//   1. Persisting evidence does not make dynamic evidence perpetually current.
//      Restored PersistedMetadata evidence is NEVER sufficient on its own to
//      establish currentness; it yields RevalidationRequired.
//   2. Process-local evidence does not survive a participant restart. A new
//      ParticipantBootId invalidates every piece of process-local evidence
//      gathered under the previous boot.
#ifndef COHERENCE_EVIDENCE_HPP
#define COHERENCE_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "coherence/enums.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/status.hpp"

namespace coherence {

/// A single, attributable observation supporting a coherence claim.
struct COHERENCE_API EvidenceRecord {
  EvidenceId id;
  EvidenceGeneration generation;
  EvidenceKind kind = EvidenceKind::None;
  EvidenceClass evidence_class = EvidenceClass::Unsupported;

  /// Who produced the observation. Nil for coordinator-internal observations.
  ParticipantId participant;
  ParticipantBootId boot;

  /// Authority epochs in force when the observation was made.
  CoordinatorEpoch epoch;
  OwnershipGeneration ownership_generation;

  /// What the observation is about.
  ObjectId object;
  ObjectGeneration object_generation;
  RegionId region;
  RegionGeneration region_generation;
  VersionId observed_version;
  ContentDigest observed_digest;
  std::uint32_t observed_crc32c = 0;

  /// Logical observation counter. Ordering is derived from this counter, never
  /// from wall-clock time or scheduling order.
  OperationSequence sequence;

  /// True when the observation is only meaningful inside the producing process
  /// lifetime (for example a mapping observation made by an agent).
  bool process_local = false;

  /// Free-form, deterministic note used by inspection output.
  std::string note;
};

/// Outcome of evaluating an evidence record against current authority.
enum class FreshnessVerdict : std::uint8_t {
  /// Evidence supports the claim right now.
  Fresh = 0,
  /// Evidence exists but no longer applies (epoch, boot, or age).
  Stale = 1,
  /// No evidence of the required kind exists at all.
  Missing = 2,
  /// The domain or backend cannot produce the required evidence.
  Unsupported = 3,
  /// The evidence is real but insufficient on its own (persisted metadata).
  RequiresRevalidation = 4,
};

COHERENCE_API std::string_view to_token(FreshnessVerdict value) noexcept;

struct COHERENCE_API FreshnessAssessment {
  FreshnessVerdict verdict = FreshnessVerdict::Missing;
  EvidenceId evidence;
  EvidenceGeneration generation;
  EvidenceClass evidence_class = EvidenceClass::Unsupported;
  /// Machine-readable explanation. Never empty for a non-Fresh verdict.
  StatusCode reason = StatusCode::EvidenceMissing;
  std::string rationale;

  [[nodiscard]] bool fresh() const noexcept { return verdict == FreshnessVerdict::Fresh; }
};

/// Everything needed to judge whether evidence is still valid.
struct COHERENCE_API EvidenceContext {
  CoordinatorEpoch current_epoch;
  /// Live boot of the participant that produced the evidence. Nil when the
  /// participant is not currently admitted.
  ParticipantBootId live_boot;
  ParticipantId participant;
  /// Current logical operation counter of the coordinator.
  OperationSequence current_sequence;
  /// Maximum accepted age of an observation in logical operations. Zero means
  /// evidence must have been produced by the operation being evaluated.
  std::uint64_t max_age_operations = 0;
  /// True when the owning region is backed by a synthetic domain. Synthetic
  /// fixtures are acceptable only there.
  bool synthetic_domain = false;
};

/// Evaluate a record. Returns Missing for a nil/default record.
[[nodiscard]] COHERENCE_API FreshnessAssessment assess_evidence(const EvidenceRecord& record,
                                                                const EvidenceContext& context);

/// Convenience used by inspection: render a record deterministically.
[[nodiscard]] COHERENCE_API std::string render_evidence(const EvidenceRecord& record);

} // namespace coherence

#endif // COHERENCE_EVIDENCE_HPP
