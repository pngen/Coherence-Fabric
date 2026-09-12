// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/evidence.hpp"

namespace coherence {
namespace {

FreshnessAssessment verdict(FreshnessVerdict v, StatusCode reason, std::string rationale) {
  FreshnessAssessment out;
  out.verdict = v;
  out.reason = reason;
  out.rationale = std::move(rationale);
  return out;
}

} // namespace

std::string_view to_token(FreshnessVerdict value) noexcept {
  switch (value) {
    case FreshnessVerdict::Fresh: return "fresh";
    case FreshnessVerdict::Stale: return "stale";
    case FreshnessVerdict::Missing: return "missing";
    case FreshnessVerdict::Unsupported: return "unsupported";
    case FreshnessVerdict::RequiresRevalidation: return "requires_revalidation";
  }
  return "unknown";
}

FreshnessAssessment assess_evidence(const EvidenceRecord& record, const EvidenceContext& context) {
  FreshnessAssessment out;
  out.evidence = record.id;
  out.generation = record.generation;
  out.evidence_class = record.evidence_class;

  if (record.kind == EvidenceKind::None || record.id.is_nil()) {
    out.verdict = FreshnessVerdict::Missing;
    out.reason = StatusCode::EvidenceMissing;
    out.rationale = "no evidence record is attached to the claim";
    return out;
  }

  if (record.evidence_class == EvidenceClass::Unsupported) {
    out.verdict = FreshnessVerdict::Unsupported;
    out.reason = StatusCode::Unsupported;
    out.rationale = "the owning backend reports that it cannot produce evidence";
    return out;
  }

  if (record.evidence_class == EvidenceClass::Synthetic && !context.synthetic_domain) {
    out.verdict = FreshnessVerdict::Unsupported;
    out.reason = StatusCode::Unsupported;
    out.rationale = "synthetic evidence cannot support a claim over a real memory domain";
    return out;
  }

  // Persisted metadata is provenance, not proof of dynamic currentness. It can
  // never, by itself, establish that a replica is current right now.
  if (record.kind == EvidenceKind::PersistedMetadata) {
    out.verdict = FreshnessVerdict::RequiresRevalidation;
    out.reason = StatusCode::RevalidationRequired;
    out.rationale =
        "persisted metadata evidence does not establish dynamic currentness; revalidation required";
    return out;
  }

  if (record.epoch != context.current_epoch) {
    out.verdict = FreshnessVerdict::Stale;
    out.reason = StatusCode::StaleEpoch;
    out.rationale = "evidence was produced under coordinator epoch " +
                    format_id("epoch", record.epoch.value()) + " but the current epoch is " +
                    format_id("epoch", context.current_epoch.value());
    return out;
  }

  if (record.process_local) {
    if (record.boot.is_nil() || context.live_boot.is_nil() || record.boot != context.live_boot) {
      out.verdict = FreshnessVerdict::Stale;
      out.reason = StatusCode::StaleBoot;
      out.rationale =
          "process-local evidence was produced under a participant boot that is no longer live";
      return out;
    }
  }

  if (record.sequence.value() > context.current_sequence.value()) {
    out.verdict = FreshnessVerdict::Stale;
    out.reason = StatusCode::InternalInvariantViolation;
    out.rationale = "evidence carries an observation sequence from the future";
    return out;
  }

  // A bound of zero means evidence is invalidated only by events (epoch
  // advance, boot change, ownership change, publication, invalidation,
  // retirement) and never merely by the passage of operations.
  const std::uint64_t age = context.current_sequence.value() - record.sequence.value();
  if (context.max_age_operations != 0 && age > context.max_age_operations) {
    out.verdict = FreshnessVerdict::Stale;
    out.reason = StatusCode::EvidenceStale;
    out.rationale = "evidence age " + std::to_string(age) +
                    " exceeds the policy bound of " +
                    std::to_string(context.max_age_operations) + " operations";
    return out;
  }

  out.verdict = FreshnessVerdict::Fresh;
  out.reason = StatusCode::Ok;
  out.rationale = "evidence is current under the active epoch and boot";
  return out;
}

std::string render_evidence(const EvidenceRecord& record) {
  std::string out;
  out.reserve(192);
  out.append("evidence ");
  out.append(format_id("ev", record.id.value()));
  out.append(" gen=");
  out.append(record.generation.to_string());
  out.append(" kind=");
  out.append(to_token(record.kind));
  out.append(" class=");
  out.append(to_token(record.evidence_class));
  out.append(" epoch=");
  out.append(record.epoch.to_string());
  out.append(" object=");
  out.append(record.object.to_string());
  out.append(" objgen=");
  out.append(record.object_generation.to_string());
  out.append(" region=");
  out.append(record.region.to_string());
  out.append(" reggen=");
  out.append(record.region_generation.to_string());
  out.append(" version=");
  out.append(record.observed_version.to_string());
  out.append(" seq=");
  out.append(record.sequence.to_string());
  if (record.process_local) out.append(" process_local");
  if (!record.note.empty()) {
    out.append(" note=\"");
    out.append(record.note);
    out.push_back('"');
  }
  return out;
}

} // namespace coherence
