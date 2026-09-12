// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/model.hpp"

#include <string>

#include "coherence/bytes.hpp"

namespace coherence {
namespace {

void append_field(std::string& out, const char* key, const std::string& value) {
  out.push_back(' ');
  out.append(key);
  out.push_back('=');
  out.append(value);
}

void append_field(std::string& out, const char* key, std::string_view value) {
  out.push_back(' ');
  out.append(key);
  out.push_back('=');
  out.append(value);
}

void append_field(std::string& out, const char* key, const char* value) {
  append_field(out, key, std::string_view(value));
}

template <typename Id>
void append_id(std::string& out, const char* key, Id id) {
  append_field(out, key, id.to_string());
}

} // namespace

ContentFingerprint fingerprint_bytes(ByteSpan bytes) {
  ContentFingerprint out;
  out.length = static_cast<std::uint64_t>(bytes.size());
  out.crc32c = crc32c(bytes);
  const auto digest = Sha256::digest(bytes);
  UInt128 value{};
  for (std::size_t i = 0; i < 8; ++i) {
    value.high = (value.high << 8) | std::to_integer<std::uint8_t>(digest[i]);
  }
  for (std::size_t i = 8; i < 16; ++i) {
    value.low = (value.low << 8) | std::to_integer<std::uint8_t>(digest[i]);
  }
  out.digest = ContentDigest::from_value(value);
  out.defined = true;
  return out;
}

std::string ContentFingerprint::to_string() const {
  if (!defined) return std::string("undefined");
  std::string out;
  out.reserve(64);
  out.append(digest.to_string());
  out.append(":crc32c=");
  out.append(std::to_string(crc32c));
  out.append(":len=");
  out.append(std::to_string(length));
  return out;
}

std::string render_object(const ObjectRecord& r) {
  std::string out;
  out.reserve(512);
  out.append("object");
  append_id(out, "id", r.id);
  append_id(out, "generation", r.generation);
  append_id(out, "domain", r.domain);
  append_field(out, "name", r.name);
  append_field(out, "length", std::to_string(r.length));
  out.push_back('\n');
  out.append("  policy=");
  out.append(r.policy.to_string());
  append_id(out, "policy_generation", r.policy_generation);
  append_field(out, "lifecycle", to_token(r.lifecycle));
  out.push_back('\n');
  out.append("  authority=");
  out.append(to_token(r.authority));
  append_id(out, "writer", r.writer);
  append_id(out, "ownership_generation", r.ownership_generation);
  out.push_back('\n');
  out.append("  authoritative_version=");
  out.append(r.authoritative_version.to_string());
  append_id(out, "publication", r.publication);
  append_field(out, "publication_state", to_token(r.publication_state));
  out.push_back('\n');
  out.append("  dirty_condition=");
  out.append(to_token(r.dirty_condition));
  append_field(out, "has_unpublished_dirty", r.has_unpublished_dirty ? "true" : "false");
  append_field(out, "replicas", std::to_string(r.replicas.size()));
  append_field(out, "reads", std::to_string(r.reads.size()));
  append_field(out, "outstanding_invalidations",
               std::to_string(r.outstanding_invalidations.size()));
  if (!r.recovery_note.empty()) {
    out.push_back('\n');
    out.append("  recovery_note=");
    out.append(r.recovery_note);
  }
  return out;
}

std::string render_region(const RegionRecord& r) {
  std::string out;
  out.reserve(512);
  out.append("region");
  append_id(out, "id", r.id);
  append_id(out, "generation", r.generation);
  append_id(out, "replica", r.replica);
  append_id(out, "replica_generation", r.replica_generation);
  append_field(out, "name", r.name);
  out.push_back('\n');
  append_field(out, "object", r.object.to_string());
  append_id(out, "object_generation", r.object_generation);
  append_field(out, "participant", r.participant.to_string());
  append_field(out, "boot", r.boot.to_string());
  out.push_back('\n');
  append_field(out, "memory_domain", to_token(r.memory_domain));
  append_field(out, "evidence_class", to_token(r.evidence_class));
  append_field(out, "lifecycle", to_token(r.lifecycle));
  append_field(out, "state", to_token(r.state));
  out.push_back('\n');
  append_field(out, "offset", std::to_string(r.offset));
  append_field(out, "length", std::to_string(r.length));
  append_field(out, "version", r.version.to_string());
  append_field(out, "last_synced_version", r.last_synced_version.to_string());
  append_field(out, "ownership_generation", r.ownership_generation.to_string());
  out.push_back('\n');
  append_field(out, "dirty", to_token(r.dirty));
  append_field(out, "invalidation_pending", r.invalidation_pending ? "true" : "false");
  append_field(out, "content", r.content.to_string());
  if (!r.note.empty()) {
    out.push_back('\n');
    append_field(out, "note", r.note);
  }
  return out;
}

std::string render_participant(const ParticipantRecord& r) {
  std::string out;
  out.reserve(256);
  out.append("participant");
  append_id(out, "id", r.id);
  append_field(out, "name", r.name);
  out.push_back('\n');
  append_field(out, "boot", r.boot.to_string());
  append_field(out, "previous_boot", r.previous_boot.to_string());
  append_field(out, "lifecycle", to_token(r.lifecycle));
  append_field(out, "live", r.live ? "true" : "false");
  out.push_back('\n');
  append_field(out, "admitted_epoch", r.admitted_epoch.to_string());
  append_field(out, "last_epoch", r.last_epoch.to_string());
  append_field(out, "regions", std::to_string(r.region_count));
  if (!r.fence_reason.empty()) {
    out.push_back('\n');
    append_field(out, "fence_reason", r.fence_reason);
  }
  return out;
}

std::string render_invalidation(const InvalidationRecord& r) {
  std::string out;
  out.reserve(320);
  out.append("invalidation");
  append_id(out, "id", r.id);
  append_field(out, "object", r.object.to_string());
  append_id(out, "object_generation", r.object_generation);
  append_field(out, "region", r.region.to_string());
  append_id(out, "region_generation", r.region_generation);
  append_id(out, "replica_generation", r.replica_generation);
  out.push_back('\n');
  append_field(out, "published_version", r.published_version.to_string());
  append_field(out, "superseded_version", r.superseded_version.to_string());
  append_field(out, "ownership_generation", r.ownership_generation.to_string());
  append_field(out, "epoch", r.epoch.to_string());
  append_field(out, "target", r.target_participant.to_string());
  append_field(out, "target_boot", r.target_boot.to_string());
  append_field(out, "state", to_token(r.state));
  append_field(out, "ack_required", r.acknowledgement_required ? "true" : "false");
  return out;
}

std::string render_sync(const SyncRecord& r) {
  std::string out;
  out.reserve(384);
  out.append("sync");
  append_id(out, "id", r.id);
  append_field(out, "kind", to_token(r.kind));
  append_field(out, "state", to_token(r.state));
  out.push_back('\n');
  append_field(out, "object", r.object.to_string());
  append_id(out, "object_generation", r.object_generation);
  append_field(out, "source", r.source_region.to_string());
  append_id(out, "source_generation", r.source_region_generation);
  append_field(out, "destination", r.destination_region.to_string());
  append_id(out, "destination_generation", r.destination_region_generation);
  out.push_back('\n');
  append_field(out, "source_version", r.source_version.to_string());
  append_field(out, "destination_prior_version", r.destination_prior_version.to_string());
  append_field(out, "ownership_generation", r.ownership_generation.to_string());
  append_field(out, "epoch", r.epoch.to_string());
  append_field(out, "transport", r.transport);
  out.push_back('\n');
  append_field(out, "expected_content", r.expected_content.to_string());
  append_field(out, "postcondition", r.postcondition);
  if (!r.failure_reason.empty()) {
    out.push_back('\n');
    append_field(out, "failure_reason", r.failure_reason);
  }
  return out;
}

} // namespace coherence
