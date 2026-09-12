// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/codec.hpp"

#include <string>
#include <vector>

#include "codec_internal.hpp"

namespace coherence {
namespace {

using codec_detail::dec_code;
using codec_detail::dec_enum;
using codec_detail::dec_header;
using codec_detail::dec_text;
using codec_detail::enc_code;
using codec_detail::enc_enum;

const DecodeLimits kLimits{};

// Validating decoders for enumerations that are only reachable through the
// binary formats. An undeclared value is rejected, never defaulted.
template <typename Enum, std::size_t N>
std::optional<Enum> from_u8(const std::pair<Enum, std::string_view> (&table)[N],
                            std::uint8_t raw) {
  const Enum candidate = static_cast<Enum>(raw);
  for (const auto& entry : table) {
    if (entry.first == candidate) return candidate;
  }
  return std::nullopt;
}

constexpr std::pair<WriteOwnershipMode, std::string_view> kOwnership[] = {
    {WriteOwnershipMode::Unspecified, "unspecified"},
    {WriteOwnershipMode::SingleWriterExclusive, "single_writer_exclusive"},
    {WriteOwnershipMode::ReadOnlyObject, "read_only_object"},
    {WriteOwnershipMode::MultiWriterUnsupported, "multi_writer_unsupported"},
};
constexpr std::pair<PublicationDurability, std::string_view> kDurability[] = {
    {PublicationDurability::Unspecified, "unspecified"},
    {PublicationDurability::Ephemeral, "ephemeral"},
    {PublicationDurability::DurableMetadata, "durable_metadata"},
};
constexpr std::pair<ConflictBehavior, std::string_view> kConflict[] = {
    {ConflictBehavior::Unspecified, "unspecified"},
    {ConflictBehavior::Reject, "reject"},
    {ConflictBehavior::InvalidateReaders, "invalidate_readers"},
};
constexpr std::pair<RecoveryPolicy, std::string_view> kRecovery[] = {
    {RecoveryPolicy::Unspecified, "unspecified"},
    {RecoveryPolicy::Conservative, "conservative"},
    {RecoveryPolicy::CompleteDurablePending, "complete_durable_pending"},
};
constexpr std::pair<StaleReadPolicy, std::string_view> kStale[] = {
    {StaleReadPolicy::Unspecified, "unspecified"},
    {StaleReadPolicy::Never, "never"},
    {StaleReadPolicy::Bounded, "bounded"},
    {StaleReadPolicy::Always, "always"},
};
constexpr std::pair<DirtyLossPolicy, std::string_view> kDirtyLoss[] = {
    {DirtyLossPolicy::Unspecified, "unspecified"},
    {DirtyLossPolicy::ReportUnknown, "report_unknown"},
    {DirtyLossPolicy::ReportLost, "report_lost"},
    {DirtyLossPolicy::RequireRecovery, "require_recovery"},
};

std::optional<WriteOwnershipMode> ownership_from_u8(std::uint8_t raw) {
  return from_u8(kOwnership, raw);
}
std::optional<PublicationDurability> durability_from_u8(std::uint8_t raw) {
  return from_u8(kDurability, raw);
}
std::optional<ConflictBehavior> conflict_from_u8(std::uint8_t raw) {
  return from_u8(kConflict, raw);
}
std::optional<RecoveryPolicy> recovery_from_u8(std::uint8_t raw) {
  return from_u8(kRecovery, raw);
}
std::optional<StaleReadPolicy> stale_from_u8(std::uint8_t raw) { return from_u8(kStale, raw); }
std::optional<DirtyLossPolicy> dirty_loss_from_u8(std::uint8_t raw) {
  return from_u8(kDirtyLoss, raw);
}

} // namespace

const DecodeLimits& default_decode_limits() noexcept { return kLimits; }

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
void encode_fingerprint(ByteWriter& w, const ContentFingerprint& value) {
  w.boolean(value.defined);
  w.strong_id(value.digest);
  w.u32(value.crc32c);
  w.u64(value.length);
}

bool decode_fingerprint(ByteReader& r, ContentFingerprint& out, const DecodeLimits& limits) {
  bool defined = false;
  if (!r.boolean(defined)) return false;
  ContentDigest digest;
  if (!r.strong_id(digest)) return false;
  std::uint32_t crc = 0;
  std::uint64_t length = 0;
  if (!r.u32(crc)) return false;
  if (!r.u64(length)) return false;
  if (length > limits.max_object_length) return false;
  out.defined = defined;
  out.digest = digest;
  out.crc32c = crc;
  out.length = length;
  return true;
}

void encode_policy(ByteWriter& w, const CoherencePolicy& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.strong_id(value.generation);
  w.text(value.name);
  enc_enum(w, value.consistency);
  enc_enum(w, value.write_ownership);
  enc_enum(w, value.publication_durability);
  enc_enum(w, value.conflict_behavior);
  enc_enum(w, value.recovery_policy);
  enc_enum(w, value.stale_read_policy);
  enc_enum(w, value.dirty_loss_policy);
  w.u64(value.stale_read_bound_versions);
  w.u64(value.evidence_max_age_operations);
  w.boolean(value.require_invalidation_acks_before_publication);
  w.boolean(value.require_sync_completion_for_currentness);
  w.u32(value.allowed_domains_mask);
  w.boolean(value.require_content_fingerprint);
}

bool decode_policy(ByteReader& r, CoherencePolicy& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  CoherencePolicy value;
  if (!r.strong_id(value.id)) return false;
  if (!r.strong_id(value.generation)) return false;
  if (!dec_text(r, value.name, limits)) return false;
  if (!dec_enum(r, value.consistency, consistency_model_from_u8)) return false;
  if (!dec_enum(r, value.write_ownership, ownership_from_u8)) return false;
  if (!dec_enum(r, value.publication_durability, durability_from_u8)) return false;
  if (!dec_enum(r, value.conflict_behavior, conflict_from_u8)) return false;
  if (!dec_enum(r, value.recovery_policy, recovery_from_u8)) return false;
  if (!dec_enum(r, value.stale_read_policy, stale_from_u8)) return false;
  if (!dec_enum(r, value.dirty_loss_policy, dirty_loss_from_u8)) return false;
  if (!r.u64(value.stale_read_bound_versions)) return false;
  if (!r.u64(value.evidence_max_age_operations)) return false;
  if (!r.boolean(value.require_invalidation_acks_before_publication)) return false;
  if (!r.boolean(value.require_sync_completion_for_currentness)) return false;
  if (!r.u32(value.allowed_domains_mask)) return false;
  if (!r.boolean(value.require_content_fingerprint)) return false;
  out = std::move(value);
  return true;
}

void encode_evidence(ByteWriter& w, const EvidenceRecord& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.strong_id(value.generation);
  enc_enum(w, value.kind);
  enc_enum(w, value.evidence_class);
  w.strong_id(value.participant);
  w.strong_id(value.boot);
  w.strong_id(value.epoch);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.observed_version);
  w.strong_id(value.observed_digest);
  w.u32(value.observed_crc32c);
  w.strong_id(value.sequence);
  w.boolean(value.process_local);
  w.text(value.note);
}

bool decode_evidence(ByteReader& r, EvidenceRecord& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  EvidenceRecord value;
  if (!r.strong_id(value.id)) return false;
  if (!r.strong_id(value.generation)) return false;
  if (!dec_enum(r, value.kind, evidence_kind_from_u8)) return false;
  if (!dec_enum(r, value.evidence_class, evidence_class_from_u8)) return false;
  if (!r.strong_id(value.participant)) return false;
  if (!r.strong_id(value.boot)) return false;
  if (!r.strong_id(value.epoch)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.object)) return false;
  if (!r.strong_id(value.object_generation)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.observed_version)) return false;
  if (!r.strong_id(value.observed_digest)) return false;
  if (!r.u32(value.observed_crc32c)) return false;
  if (!r.strong_id(value.sequence)) return false;
  if (!r.boolean(value.process_local)) return false;
  if (!dec_text(r, value.note, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_authority_context(ByteWriter& w, const AuthorityContext& value) {
  w.strong_id(value.epoch);
  w.strong_id(value.participant);
  w.strong_id(value.boot);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.policy_generation);
  w.strong_id(value.request);
}

bool decode_authority_context(ByteReader& r, AuthorityContext& out, const DecodeLimits&) {
  AuthorityContext value;
  if (!r.strong_id(value.epoch)) return false;
  if (!r.strong_id(value.participant)) return false;
  if (!r.strong_id(value.boot)) return false;
  if (!r.strong_id(value.object)) return false;
  if (!r.strong_id(value.object_generation)) return false;
  if (!r.strong_id(value.policy_generation)) return false;
  if (!r.strong_id(value.request)) return false;
  out = value;
  return true;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------
void encode_domain(ByteWriter& w, const DomainRecord& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.text(value.name);
  w.u64(value.generation);
  enc_enum(w, value.lifecycle);
  w.strong_id(value.created_epoch);
  w.strong_id(value.current_epoch);
}

bool decode_domain(ByteReader& r, DomainRecord& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  DomainRecord value;
  if (!r.strong_id(value.id)) return false;
  if (!dec_text(r, value.name, limits)) return false;
  if (!r.u64(value.generation)) return false;
  if (!dec_enum(r, value.lifecycle, domain_lifecycle_from_u8)) return false;
  if (!r.strong_id(value.created_epoch)) return false;
  if (!r.strong_id(value.current_epoch)) return false;
  if (value.generation == 0) return false;
  out = std::move(value);
  return true;
}

void encode_participant(ByteWriter& w, const ParticipantRecord& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.text(value.name);
  w.strong_id(value.boot);
  w.strong_id(value.previous_boot);
  enc_enum(w, value.lifecycle);
  w.strong_id(value.admitted_epoch);
  w.strong_id(value.last_epoch);
  w.strong_id(value.session);
  w.strong_id(value.last_sequence);
  w.boolean(value.live);
  w.u64(value.region_count);
  w.text(value.node_label);
  w.text(value.fence_reason);
}

bool decode_participant(ByteReader& r, ParticipantRecord& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  ParticipantRecord value;
  if (!r.strong_id(value.id)) return false;
  if (!dec_text(r, value.name, limits)) return false;
  if (!r.strong_id(value.boot)) return false;
  if (!r.strong_id(value.previous_boot)) return false;
  if (!dec_enum(r, value.lifecycle, participant_lifecycle_from_u8)) return false;
  if (!r.strong_id(value.admitted_epoch)) return false;
  if (!r.strong_id(value.last_epoch)) return false;
  if (!r.strong_id(value.session)) return false;
  if (!r.strong_id(value.last_sequence)) return false;
  if (!r.boolean(value.live)) return false;
  if (!r.u64(value.region_count)) return false;
  if (!dec_text(r, value.node_label, limits)) return false;
  if (!dec_text(r, value.fence_reason, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_read_grant(ByteWriter& w, const ReadGrant& value) {
  w.strong_id(value.lease);
  w.strong_id(value.participant);
  w.strong_id(value.boot);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.version);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.granted_sequence);
  w.boolean(value.released);
}

bool decode_read_grant(ByteReader& r, ReadGrant& out) {
  ReadGrant value;
  if (!r.strong_id(value.lease)) return false;
  if (!r.strong_id(value.participant)) return false;
  if (!r.strong_id(value.boot)) return false;
  if (!r.strong_id(value.object)) return false;
  if (!r.strong_id(value.object_generation)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.version)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.granted_sequence)) return false;
  if (!r.boolean(value.released)) return false;
  out = value;
  return true;
}

void encode_object(ByteWriter& w, const ObjectRecord& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.strong_id(value.generation);
  w.strong_id(value.domain);
  w.text(value.name);
  w.u64(value.length);
  w.strong_id(value.policy);
  w.strong_id(value.policy_generation);
  enc_enum(w, value.lifecycle);
  enc_enum(w, value.authority);
  w.strong_id(value.writer);
  w.strong_id(value.writer_boot);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.authoritative_version);
  w.strong_id(value.published_version);
  w.strong_id(value.publication);
  w.strong_id(value.committed_sequence);
  w.strong_id(value.pending_publication);
  w.strong_id(value.pending_version);
  enc_enum(w, value.publication_state);
  encode_fingerprint(w, value.pending_content);
  w.strong_id(value.preferred_current_replica);
  w.u64(value.replicas.size());
  for (const RegionId id : value.replicas) w.strong_id(id);
  w.u64(value.reads.size());
  for (const ReadGrant& grant : value.reads) encode_read_grant(w, grant);
  w.u64(value.outstanding_invalidations.size());
  for (const InvalidationId id : value.outstanding_invalidations) w.strong_id(id);
  w.u64(value.completed_requests.size());
  for (const CompletedPublicationRequest& entry : value.completed_requests) {
    w.strong_id(entry.request);
    w.strong_id(entry.version);
    w.strong_id(entry.publication);
    w.strong_id(entry.committed_sequence);
  }
  w.strong_id(value.last_writer);
  w.strong_id(value.last_writer_boot);
  w.boolean(value.has_unpublished_dirty);
  enc_enum(w, value.dirty_condition);
  w.strong_id(value.updated_sequence);
  w.text(value.recovery_note);
}

bool decode_object(ByteReader& r, ObjectRecord& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  ObjectRecord value;
  if (!r.strong_id(value.id)) return false;
  if (!r.strong_id(value.generation)) return false;
  if (!r.strong_id(value.domain)) return false;
  if (!dec_text(r, value.name, limits)) return false;
  if (!r.u64(value.length)) return false;
  if (value.length > limits.max_object_length) return false;
  if (!r.strong_id(value.policy)) return false;
  if (!r.strong_id(value.policy_generation)) return false;
  if (!dec_enum(r, value.lifecycle, object_lifecycle_from_u8)) return false;
  if (!dec_enum(r, value.authority, authority_mode_from_u8)) return false;
  if (!r.strong_id(value.writer)) return false;
  if (!r.strong_id(value.writer_boot)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.authoritative_version)) return false;
  if (!r.strong_id(value.published_version)) return false;
  if (!r.strong_id(value.publication)) return false;
  if (!r.strong_id(value.committed_sequence)) return false;
  if (!r.strong_id(value.pending_publication)) return false;
  if (!r.strong_id(value.pending_version)) return false;
  if (!dec_enum(r, value.publication_state, publication_state_from_u8)) return false;
  if (!decode_fingerprint(r, value.pending_content, limits)) return false;

  if (!r.strong_id(value.preferred_current_replica)) return false;
  std::uint64_t replica_count = 0;
  if (!r.u64(replica_count)) return false;
  if (replica_count > limits.max_collection) return false;
  value.replicas.resize(static_cast<std::size_t>(replica_count));
  for (std::uint64_t i = 0; i < replica_count; ++i) {
    if (!r.strong_id(value.replicas[static_cast<std::size_t>(i)])) return false;
  }

  std::uint64_t read_count = 0;
  if (!r.u64(read_count)) return false;
  if (read_count > limits.max_collection) return false;
  value.reads.resize(static_cast<std::size_t>(read_count));
  for (std::uint64_t i = 0; i < read_count; ++i) {
    if (!decode_read_grant(r, value.reads[static_cast<std::size_t>(i)])) return false;
  }

  std::uint64_t invalidation_count = 0;
  if (!r.u64(invalidation_count)) return false;
  if (invalidation_count > limits.max_collection) return false;
  value.outstanding_invalidations.resize(static_cast<std::size_t>(invalidation_count));
  for (std::uint64_t i = 0; i < invalidation_count; ++i) {
    if (!r.strong_id(value.outstanding_invalidations[static_cast<std::size_t>(i)])) return false;
  }

  std::uint64_t completed_count = 0;
  if (!r.u64(completed_count)) return false;
  if (completed_count > limits.max_collection) return false;
  value.completed_requests.resize(static_cast<std::size_t>(completed_count));
  for (std::uint64_t i = 0; i < completed_count; ++i) {
    CompletedPublicationRequest& entry = value.completed_requests[static_cast<std::size_t>(i)];
    if (!r.strong_id(entry.request)) return false;
    if (!r.strong_id(entry.version)) return false;
    if (!r.strong_id(entry.publication)) return false;
    if (!r.strong_id(entry.committed_sequence)) return false;
  }

  if (!r.strong_id(value.last_writer)) return false;
  if (!r.strong_id(value.last_writer_boot)) return false;
  if (!r.boolean(value.has_unpublished_dirty)) return false;
  if (!dec_enum(r, value.dirty_condition, dirty_condition_from_u8)) return false;
  if (!r.strong_id(value.updated_sequence)) return false;
  if (!dec_text(r, value.recovery_note, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_region(ByteWriter& w, const RegionRecord& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.strong_id(value.generation);
  w.strong_id(value.replica);
  w.strong_id(value.replica_generation);
  w.strong_id(value.domain);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.participant);
  w.strong_id(value.boot);
  enc_enum(w, value.memory_domain);
  enc_enum(w, value.evidence_class);
  w.text(value.name);
  w.u64(value.offset);
  w.u64(value.length);
  w.u64(value.address_hint);
  w.boolean(value.declared_writable);
  enc_enum(w, value.lifecycle);
  enc_enum(w, value.state);
  w.strong_id(value.version);
  w.strong_id(value.last_synced_version);
  w.strong_id(value.last_published_version);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.last_invalidation);
  w.strong_id(value.pending_invalidation);
  w.strong_id(value.pending_invalidation_target);
  w.boolean(value.invalidation_pending);
  w.strong_id(value.evidence);
  w.strong_id(value.evidence_generation);
  enc_enum(w, value.dirty);
  w.strong_id(value.dirty_base_version);
  w.boolean(value.dirty_published);
  encode_fingerprint(w, value.content);
  w.strong_id(value.updated_sequence);
  w.text(value.note);
}

bool decode_region(ByteReader& r, RegionRecord& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  RegionRecord value;
  if (!r.strong_id(value.id)) return false;
  if (!r.strong_id(value.generation)) return false;
  if (!r.strong_id(value.replica)) return false;
  if (!r.strong_id(value.replica_generation)) return false;
  if (!r.strong_id(value.domain)) return false;
  if (!r.strong_id(value.object)) return false;
  if (!r.strong_id(value.object_generation)) return false;
  if (!r.strong_id(value.participant)) return false;
  if (!r.strong_id(value.boot)) return false;
  if (!dec_enum(r, value.memory_domain, memory_domain_from_u8)) return false;
  if (!dec_enum(r, value.evidence_class, evidence_class_from_u8)) return false;
  if (!dec_text(r, value.name, limits)) return false;
  if (!r.u64(value.offset)) return false;
  if (!r.u64(value.length)) return false;
  if (!r.u64(value.address_hint)) return false;
  if (value.length > limits.max_object_length) return false;
  if (!r.boolean(value.declared_writable)) return false;
  if (!dec_enum(r, value.lifecycle, region_lifecycle_from_u8)) return false;
  if (!dec_enum(r, value.state, coherence_state_from_u8)) return false;
  if (!r.strong_id(value.version)) return false;
  if (!r.strong_id(value.last_synced_version)) return false;
  if (!r.strong_id(value.last_published_version)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.last_invalidation)) return false;
  if (!r.strong_id(value.pending_invalidation)) return false;
  if (!r.strong_id(value.pending_invalidation_target)) return false;
  if (!r.boolean(value.invalidation_pending)) return false;
  if (!r.strong_id(value.evidence)) return false;
  if (!r.strong_id(value.evidence_generation)) return false;
  if (!dec_enum(r, value.dirty, dirty_condition_from_u8)) return false;
  if (!r.strong_id(value.dirty_base_version)) return false;
  if (!r.boolean(value.dirty_published)) return false;
  if (!decode_fingerprint(r, value.content, limits)) return false;
  if (!r.strong_id(value.updated_sequence)) return false;
  if (!dec_text(r, value.note, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_invalidation(ByteWriter& w, const InvalidationRecord& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.replica_generation);
  w.strong_id(value.published_version);
  w.strong_id(value.superseded_version);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.epoch);
  w.strong_id(value.target_participant);
  w.strong_id(value.target_boot);
  enc_enum(w, value.state);
  w.boolean(value.acknowledgement_required);
  w.strong_id(value.issued_sequence);
  w.strong_id(value.settled_sequence);
  w.text(value.rationale);
}

bool decode_invalidation(ByteReader& r, InvalidationRecord& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  InvalidationRecord value;
  if (!r.strong_id(value.id)) return false;
  if (!r.strong_id(value.object)) return false;
  if (!r.strong_id(value.object_generation)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.replica_generation)) return false;
  if (!r.strong_id(value.published_version)) return false;
  if (!r.strong_id(value.superseded_version)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.epoch)) return false;
  if (!r.strong_id(value.target_participant)) return false;
  if (!r.strong_id(value.target_boot)) return false;
  if (!dec_enum(r, value.state, invalidation_state_from_u8)) return false;
  if (!r.boolean(value.acknowledgement_required)) return false;
  if (!r.strong_id(value.issued_sequence)) return false;
  if (!r.strong_id(value.settled_sequence)) return false;
  if (!dec_text(r, value.rationale, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_sync(ByteWriter& w, const SyncRecord& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  enc_enum(w, value.kind);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.source_region);
  w.strong_id(value.source_region_generation);
  w.strong_id(value.destination_region);
  w.strong_id(value.destination_region_generation);
  w.strong_id(value.source_version);
  w.strong_id(value.destination_prior_version);
  w.u64(value.extent_offset);
  w.u64(value.extent_length);
  encode_fingerprint(w, value.expected_content);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.policy);
  w.strong_id(value.policy_generation);
  w.strong_id(value.epoch);
  w.strong_id(value.destination_participant);
  w.strong_id(value.destination_boot);
  enc_enum(w, value.state);
  w.text(value.postcondition);
  w.text(value.transport);
  w.strong_id(value.issued_sequence);
  w.strong_id(value.settled_sequence);
  w.text(value.failure_reason);
}

bool decode_sync(ByteReader& r, SyncRecord& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  SyncRecord value;
  if (!r.strong_id(value.id)) return false;
  if (!dec_enum(r, value.kind, sync_kind_from_u8)) return false;
  if (!r.strong_id(value.object)) return false;
  if (!r.strong_id(value.object_generation)) return false;
  if (!r.strong_id(value.source_region)) return false;
  if (!r.strong_id(value.source_region_generation)) return false;
  if (!r.strong_id(value.destination_region)) return false;
  if (!r.strong_id(value.destination_region_generation)) return false;
  if (!r.strong_id(value.source_version)) return false;
  if (!r.strong_id(value.destination_prior_version)) return false;
  if (!r.u64(value.extent_offset)) return false;
  if (!r.u64(value.extent_length)) return false;
  if (value.extent_length > limits.max_object_length) return false;
  if (!decode_fingerprint(r, value.expected_content, limits)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.policy)) return false;
  if (!r.strong_id(value.policy_generation)) return false;
  if (!r.strong_id(value.epoch)) return false;
  if (!r.strong_id(value.destination_participant)) return false;
  if (!r.strong_id(value.destination_boot)) return false;
  if (!dec_enum(r, value.state, sync_state_from_u8)) return false;
  if (!dec_text(r, value.postcondition, limits)) return false;
  if (!dec_text(r, value.transport, limits)) return false;
  if (!r.strong_id(value.issued_sequence)) return false;
  if (!r.strong_id(value.settled_sequence)) return false;
  if (!dec_text(r, value.failure_reason, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_pending_publication(ByteWriter& w, const PendingPublication& value) {
  w.u16(kRecordVersion);
  w.strong_id(value.id);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.version);
  w.strong_id(value.prior_version);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.epoch);
  w.strong_id(value.writer);
  w.strong_id(value.writer_boot);
  w.strong_id(value.request);
  encode_fingerprint(w, value.content);
  w.strong_id(value.sequence);
  w.boolean(value.durable);
}

bool decode_pending_publication(ByteReader& r, PendingPublication& out, const DecodeLimits& limits) {
  if (!dec_header(r, kRecordVersion)) return false;
  PendingPublication value;
  if (!r.strong_id(value.id)) return false;
  if (!r.strong_id(value.object)) return false;
  if (!r.strong_id(value.object_generation)) return false;
  if (!r.strong_id(value.version)) return false;
  if (!r.strong_id(value.prior_version)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.epoch)) return false;
  if (!r.strong_id(value.writer)) return false;
  if (!r.strong_id(value.writer_boot)) return false;
  if (!r.strong_id(value.request)) return false;
  if (!decode_fingerprint(r, value.content, limits)) return false;
  if (!r.strong_id(value.sequence)) return false;
  if (!r.boolean(value.durable)) return false;
  out = std::move(value);
  return true;
}

} // namespace coherence
