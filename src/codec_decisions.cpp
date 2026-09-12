// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Codecs for decisions, outcomes and request payloads.
#include <optional>
#include <string>

#include "coherence/codec.hpp"

#include "codec_internal.hpp"

namespace coherence {
namespace {

using codec_detail::dec_code;
using codec_detail::dec_enum;
using codec_detail::dec_header;
using codec_detail::dec_text;
using codec_detail::enc_code;
using codec_detail::enc_enum;

constexpr std::uint16_t kVersion = kRecordVersion;

void enc_context(ByteWriter& w, const DecisionContext& value) {
  w.strong_id(value.decision);
  w.strong_id(value.epoch);
  w.strong_id(value.policy);
  w.strong_id(value.policy_generation);
  w.strong_id(value.object);
  w.strong_id(value.object_generation);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.sequence);
}

bool dec_context(ByteReader& r, DecisionContext& out) {
  if (!r.strong_id(out.decision)) return false;
  if (!r.strong_id(out.epoch)) return false;
  if (!r.strong_id(out.policy)) return false;
  if (!r.strong_id(out.policy_generation)) return false;
  if (!r.strong_id(out.object)) return false;
  if (!r.strong_id(out.object_generation)) return false;
  if (!r.strong_id(out.ownership_generation)) return false;
  if (!r.strong_id(out.sequence)) return false;
  return true;
}

std::optional<SyncOperationKind> kind_from_u8(std::uint8_t raw) { return sync_kind_from_u8(raw); }

} // namespace

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------
void encode_read_decision(ByteWriter& w, const ReadDecision& value) {
  w.u16(kVersion);
  enc_context(w, value.context);
  w.u8(static_cast<std::uint8_t>(value.outcome));
  enc_code(w, value.reason);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.u8(static_cast<std::uint8_t>(value.region_state));
  w.strong_id(value.authoritative_version);
  w.strong_id(value.region_version);
  w.u64(value.staleness);
  w.boolean(value.staleness_known);
  w.boolean(value.sync_required);
  w.boolean(value.stale_allowed);
  w.boolean(value.evidence_fresh);
  w.strong_id(value.evidence);
  w.strong_id(value.evidence_generation);
  w.u8(static_cast<std::uint8_t>(value.evidence_class));
  w.strong_id(value.required_sync);
  w.strong_id(value.lease);
  w.text(value.rationale);
}

bool decode_read_decision(ByteReader& r, ReadDecision& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  ReadDecision value;
  if (!dec_context(r, value.context)) return false;
  if (!dec_enum(r, value.outcome, read_outcome_from_u8)) return false;
  if (!dec_code(r, value.reason)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!dec_enum(r, value.region_state, coherence_state_from_u8)) return false;
  if (!r.strong_id(value.authoritative_version)) return false;
  if (!r.strong_id(value.region_version)) return false;
  if (!r.u64(value.staleness)) return false;
  if (!r.boolean(value.staleness_known)) return false;
  if (!r.boolean(value.sync_required)) return false;
  if (!r.boolean(value.stale_allowed)) return false;
  if (!r.boolean(value.evidence_fresh)) return false;
  if (!r.strong_id(value.evidence)) return false;
  if (!r.strong_id(value.evidence_generation)) return false;
  if (!dec_enum(r, value.evidence_class, evidence_class_from_u8)) return false;
  if (!r.strong_id(value.required_sync)) return false;
  if (!r.strong_id(value.lease)) return false;
  if (!dec_text(r, value.rationale, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_write_grant(ByteWriter& w, const WriteGrant& value) {
  w.u16(kVersion);
  enc_context(w, value.context);
  w.boolean(value.granted);
  enc_code(w, value.reason);
  w.strong_id(value.writer);
  w.strong_id(value.writer_boot);
  w.strong_id(value.base_version);
  w.strong_id(value.authoritative_version);
  w.strong_id(value.writer_region);
  w.strong_id(value.writer_region_generation);
  w.u64(value.required_invalidations.size());
  for (const InvalidationRecord& record : value.required_invalidations) {
    encode_invalidation(w, record);
  }
  w.boolean(value.may_mutate_now);
  w.text(value.rationale);
}

bool decode_write_grant(ByteReader& r, WriteGrant& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  WriteGrant value;
  if (!dec_context(r, value.context)) return false;
  if (!r.boolean(value.granted)) return false;
  if (!dec_code(r, value.reason)) return false;
  if (!r.strong_id(value.writer)) return false;
  if (!r.strong_id(value.writer_boot)) return false;
  if (!r.strong_id(value.base_version)) return false;
  if (!r.strong_id(value.authoritative_version)) return false;
  if (!r.strong_id(value.writer_region)) return false;
  if (!r.strong_id(value.writer_region_generation)) return false;
  std::uint64_t count = 0;
  if (!r.u64(count)) return false;
  if (count > limits.max_collection) return false;
  value.required_invalidations.resize(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    if (!decode_invalidation(r, value.required_invalidations[static_cast<std::size_t>(i)], limits)) {
      return false;
    }
  }
  if (!r.boolean(value.may_mutate_now)) return false;
  if (!dec_text(r, value.rationale, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_publication(ByteWriter& w, const PublicationReceipt& value) {
  w.u16(kVersion);
  enc_context(w, value.context);
  w.strong_id(value.publication);
  w.u8(static_cast<std::uint8_t>(value.state));
  enc_code(w, value.reason);
  w.strong_id(value.version);
  w.strong_id(value.prior_version);
  w.strong_id(value.writer);
  w.strong_id(value.writer_boot);
  encode_fingerprint(w, value.content);
  w.u8(static_cast<std::uint8_t>(value.durability));
  w.boolean(value.durable);
  w.boolean(value.idempotent_replay);
  w.boolean(value.completed_by_recovery);
  w.text(value.rationale);
}

bool decode_publication(ByteReader& r, PublicationReceipt& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  PublicationReceipt value;
  if (!dec_context(r, value.context)) return false;
  if (!r.strong_id(value.publication)) return false;
  if (!dec_enum(r, value.state, publication_state_from_u8)) return false;
  if (!dec_code(r, value.reason)) return false;
  if (!r.strong_id(value.version)) return false;
  if (!r.strong_id(value.prior_version)) return false;
  if (!r.strong_id(value.writer)) return false;
  if (!r.strong_id(value.writer_boot)) return false;
  if (!decode_fingerprint(r, value.content, limits)) return false;
  if (!dec_enum(r, value.durability, [](std::uint8_t raw) -> std::optional<PublicationDurability> {
        switch (raw) {
          case 0: return PublicationDurability::Unspecified;
          case 1: return PublicationDurability::Ephemeral;
          case 2: return PublicationDurability::DurableMetadata;
          default: return std::nullopt;
        }
      })) {
    return false;
  }
  if (!r.boolean(value.durable)) return false;
  if (!r.boolean(value.idempotent_replay)) return false;
  if (!r.boolean(value.completed_by_recovery)) return false;
  if (!dec_text(r, value.rationale, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_invalidation_outcome(ByteWriter& w, const InvalidationOutcome& value) {
  w.u16(kVersion);
  enc_context(w, value.context);
  w.strong_id(value.invalidation);
  w.u8(static_cast<std::uint8_t>(value.state));
  enc_code(w, value.reason);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.replica_generation);
  w.boolean(value.idempotent_replay);
  w.text(value.rationale);
}

bool decode_invalidation_outcome(ByteReader& r, InvalidationOutcome& out,
                                 const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  InvalidationOutcome value;
  if (!dec_context(r, value.context)) return false;
  if (!r.strong_id(value.invalidation)) return false;
  if (!dec_enum(r, value.state, invalidation_state_from_u8)) return false;
  if (!dec_code(r, value.reason)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.replica_generation)) return false;
  if (!r.boolean(value.idempotent_replay)) return false;
  if (!dec_text(r, value.rationale, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_sync_outcome(ByteWriter& w, const SyncOutcome& value) {
  w.u16(kVersion);
  enc_context(w, value.context);
  w.strong_id(value.operation);
  w.u8(static_cast<std::uint8_t>(value.state));
  enc_code(w, value.reason);
  encode_sync(w, value.plan);
  w.boolean(value.idempotent_replay);
  w.text(value.rationale);
}

bool decode_sync_outcome(ByteReader& r, SyncOutcome& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  SyncOutcome value;
  if (!dec_context(r, value.context)) return false;
  if (!r.strong_id(value.operation)) return false;
  if (!dec_enum(r, value.state, sync_state_from_u8)) return false;
  if (!dec_code(r, value.reason)) return false;
  if (!decode_sync(r, value.plan, limits)) return false;
  if (!r.boolean(value.idempotent_replay)) return false;
  if (!dec_text(r, value.rationale, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_release_outcome(ByteWriter& w, const ReleaseOutcome& value) {
  w.u16(kVersion);
  enc_context(w, value.context);
  enc_code(w, value.reason);
  w.strong_id(value.lease);
  w.strong_id(value.participant);
  w.strong_id(value.boot);
  w.strong_id(value.version);
  w.boolean(value.already_released);
  w.text(value.rationale);
}

bool decode_release_outcome(ByteReader& r, ReleaseOutcome& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  ReleaseOutcome value;
  if (!dec_context(r, value.context)) return false;
  if (!dec_code(r, value.reason)) return false;
  if (!r.strong_id(value.lease)) return false;
  if (!r.strong_id(value.participant)) return false;
  if (!r.strong_id(value.boot)) return false;
  if (!r.strong_id(value.version)) return false;
  if (!r.boolean(value.already_released)) return false;
  if (!dec_text(r, value.rationale, limits)) return false;
  out = std::move(value);
  return true;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------
void encode_read_request(ByteWriter& w, const ReadRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.snapshot_version);
  w.boolean(value.require_current);
}

bool decode_read_request(ByteReader& r, ReadRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  ReadRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.snapshot_version)) return false;
  if (!r.boolean(value.require_current)) return false;
  out = value;
  return true;
}

void encode_write_request(ByteWriter& w, const WriteRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
}

bool decode_write_request(ByteReader& r, WriteRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  WriteRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  out = value;
  return true;
}

void encode_dirty_request(ByteWriter& w, const DirtyRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.base_version);
  encode_fingerprint(w, value.content);
}

bool decode_dirty_request(ByteReader& r, DirtyRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  DirtyRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.base_version)) return false;
  if (!decode_fingerprint(r, value.content, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_publish_request(ByteWriter& w, const PublishRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.expected_base_version);
  encode_fingerprint(w, value.content);
  w.boolean(value.allow_without_dirty);
}

bool decode_publish_request(ByteReader& r, PublishRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  PublishRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.expected_base_version)) return false;
  if (!decode_fingerprint(r, value.content, limits)) return false;
  if (!r.boolean(value.allow_without_dirty)) return false;
  out = std::move(value);
  return true;
}

void encode_release_request(ByteWriter& w, const ReleaseRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.lease);
  w.strong_id(value.ownership_generation);
  w.boolean(value.release_write_authority);
}

bool decode_release_request(ByteReader& r, ReleaseRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  ReleaseRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.lease)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.boolean(value.release_write_authority)) return false;
  out = value;
  return true;
}

void encode_invalidation_request(ByteWriter& w, const InvalidationRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.target_region);
  w.strong_id(value.target_region_generation);
  w.strong_id(value.target_replica_generation);
  w.strong_id(value.ownership_generation);
  w.strong_id(value.published_version);
  w.boolean(value.acknowledgement_required);
}

bool decode_invalidation_request(ByteReader& r, InvalidationRequest& out,
                                 const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  InvalidationRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.target_region)) return false;
  if (!r.strong_id(value.target_region_generation)) return false;
  if (!r.strong_id(value.target_replica_generation)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!r.strong_id(value.published_version)) return false;
  if (!r.boolean(value.acknowledgement_required)) return false;
  out = value;
  return true;
}

void encode_invalidation_ack(ByteWriter& w, const InvalidationAck& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.invalidation);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.replica_generation);
  w.strong_id(value.superseded_version);
}

bool decode_invalidation_ack(ByteReader& r, InvalidationAck& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  InvalidationAck value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.invalidation)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.replica_generation)) return false;
  if (!r.strong_id(value.superseded_version)) return false;
  out = value;
  return true;
}

void encode_sync_request(ByteWriter& w, const SyncRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.u8(static_cast<std::uint8_t>(value.kind));
  w.strong_id(value.source_region);
  w.strong_id(value.source_region_generation);
  w.strong_id(value.destination_region);
  w.strong_id(value.destination_region_generation);
  w.strong_id(value.ownership_generation);
  w.text(value.transport);
}

bool decode_sync_request(ByteReader& r, SyncRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  SyncRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!dec_enum(r, value.kind, kind_from_u8)) return false;
  if (!r.strong_id(value.source_region)) return false;
  if (!r.strong_id(value.source_region_generation)) return false;
  if (!r.strong_id(value.destination_region)) return false;
  if (!r.strong_id(value.destination_region_generation)) return false;
  if (!r.strong_id(value.ownership_generation)) return false;
  if (!dec_text(r, value.transport, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_sync_complete(ByteWriter& w, const SyncCompleteRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.operation);
  w.strong_id(value.destination_region);
  w.strong_id(value.destination_region_generation);
  w.strong_id(value.destination_new_version);
  encode_fingerprint(w, value.observed_content);
  w.boolean(value.content_moved);
}

bool decode_sync_complete(ByteReader& r, SyncCompleteRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  SyncCompleteRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.operation)) return false;
  if (!r.strong_id(value.destination_region)) return false;
  if (!r.strong_id(value.destination_region_generation)) return false;
  if (!r.strong_id(value.destination_new_version)) return false;
  if (!decode_fingerprint(r, value.observed_content, limits)) return false;
  if (!r.boolean(value.content_moved)) return false;
  out = std::move(value);
  return true;
}

void encode_sync_fail(ByteWriter& w, const SyncFailRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.operation);
  enc_code(w, value.cause);
  w.boolean(value.outcome_unknown);
  w.text(value.detail);
}

bool decode_sync_fail(ByteReader& r, SyncFailRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  SyncFailRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.operation)) return false;
  if (!dec_code(r, value.cause)) return false;
  if (!r.boolean(value.outcome_unknown)) return false;
  if (!dec_text(r, value.detail, limits)) return false;
  out = std::move(value);
  return true;
}

void encode_revalidate_request(ByteWriter& w, const RevalidateRequest& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.region);
  w.strong_id(value.region_generation);
  w.strong_id(value.observed_version);
  encode_fingerprint(w, value.content);
  w.boolean(value.byte_compared);
}

bool decode_revalidate_request(ByteReader& r, RevalidateRequest& out, const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  RevalidateRequest value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.region)) return false;
  if (!r.strong_id(value.region_generation)) return false;
  if (!r.strong_id(value.observed_version)) return false;
  if (!decode_fingerprint(r, value.content, limits)) return false;
  if (!r.boolean(value.byte_compared)) return false;
  out = std::move(value);
  return true;
}

void encode_region_registration(ByteWriter& w, const RegionRegistration& value) {
  w.u16(kVersion);
  encode_authority_context(w, value.context);
  w.strong_id(value.domain);
  w.strong_id(value.requested_id);
  w.text(value.name);
  w.u8(static_cast<std::uint8_t>(value.memory_domain));
  w.u8(static_cast<std::uint8_t>(value.evidence_class));
  w.u64(value.offset);
  w.u64(value.length);
  w.u64(value.address_hint);
  w.strong_id(value.requested_replica);
  w.boolean(value.declared_writable);
  w.strong_id(value.initial_version);
  encode_fingerprint(w, value.content);
}

bool decode_region_registration(ByteReader& r, RegionRegistration& out,
                                const DecodeLimits& limits) {
  std::uint16_t version = 0;
  if (!r.u16(version) || version != kVersion) return false;
  RegionRegistration value;
  if (!decode_authority_context(r, value.context, limits)) return false;
  if (!r.strong_id(value.domain)) return false;
  if (!r.strong_id(value.requested_id)) return false;
  if (!dec_text(r, value.name, limits)) return false;
  if (!dec_enum(r, value.memory_domain, memory_domain_from_u8)) return false;
  if (!dec_enum(r, value.evidence_class, evidence_class_from_u8)) return false;
  if (!r.u64(value.offset)) return false;
  if (!r.u64(value.length)) return false;
  if (!r.u64(value.address_hint)) return false;
  if (value.length > limits.max_object_length) return false;
  if (!r.strong_id(value.requested_replica)) return false;
  if (!r.boolean(value.declared_writable)) return false;
  if (!r.strong_id(value.initial_version)) return false;
  if (!decode_fingerprint(r, value.content, limits)) return false;
  out = std::move(value);
  return true;
}

} // namespace coherence
