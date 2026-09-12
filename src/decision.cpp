// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/decision.hpp"

#include <string>

namespace coherence {
namespace {

void field(std::string& out, const char* key, const std::string& value) {
  out.push_back(' ');
  out.append(key);
  out.push_back('=');
  out.append(value);
}

void field(std::string& out, const char* key, std::string_view value) {
  out.push_back(' ');
  out.append(key);
  out.push_back('=');
  out.append(value);
}

void field(std::string& out, const char* key, const char* value) {
  field(out, key, std::string_view(value));
}

void append_context(std::string& out, const DecisionContext& context) {
  out.append("decision=");
  out.append(context.decision.to_string());
  field(out, "epoch", context.epoch.to_string());
  field(out, "object", context.object.to_string());
  field(out, "object_generation", context.object_generation.to_string());
  field(out, "ownership_generation", context.ownership_generation.to_string());
  field(out, "policy", context.policy.to_string());
  field(out, "policy_generation", context.policy_generation.to_string());
  field(out, "sequence", context.sequence.to_string());
}

} // namespace

std::string render_read_decision(const ReadDecision& d) {
  std::string out;
  out.reserve(640);
  out.append("read_decision outcome=");
  out.append(to_token(d.outcome));
  out.append(" reason=");
  out.append(status_code_name(d.reason));
  out.push_back('\n');
  out.append("  ");
  append_context(out, d.context);
  out.push_back('\n');
  field(out, " region", d.region.to_string());
  field(out, "region_generation", d.region_generation.to_string());
  field(out, "region_state", to_token(d.region_state));
  out.push_back('\n');
  field(out, " authoritative_version", d.authoritative_version.to_string());
  field(out, "region_version", d.region_version.to_string());
  if (d.staleness_known) {
    field(out, "staleness", std::to_string(d.staleness));
  } else {
    field(out, "staleness", "unknown");
  }
  out.push_back('\n');
  field(out, " sync_required", d.sync_required ? "true" : "false");
  field(out, "stale_allowed", d.stale_allowed ? "true" : "false");
  field(out, "evidence_fresh", d.evidence_fresh ? "true" : "false");
  field(out, "evidence_class", to_token(d.evidence_class));
  out.push_back('\n');
  if (!d.evidence.is_nil()) {
    field(out, " evidence", d.evidence.to_string());
    field(out, "evidence_generation", d.evidence_generation.to_string());
  }
  if (!d.required_sync.is_nil()) field(out, " required_sync", d.required_sync.to_string());
  if (!d.lease.is_nil()) field(out, " lease", d.lease.to_string());
  out.push_back('\n');
  out.append("  rationale=");
  out.append(d.rationale);
  return out;
}

std::string render_write_grant(const WriteGrant& g) {
  std::string out;
  out.reserve(640);
  out.append("write_grant granted=");
  out.append(g.granted ? "true" : "false");
  out.append(" reason=");
  out.append(status_code_name(g.reason));
  out.push_back('\n');
  out.append("  ");
  append_context(out, g.context);
  out.push_back('\n');
  field(out, " writer", g.writer.to_string());
  field(out, "writer_boot", g.writer_boot.to_string());
  field(out, "base_version", g.base_version.to_string());
  field(out, "authoritative_version", g.authoritative_version.to_string());
  out.push_back('\n');
  field(out, " writer_region", g.writer_region.to_string());
  field(out, "writer_region_generation", g.writer_region_generation.to_string());
  field(out, "may_mutate_now", g.may_mutate_now ? "true" : "false");
  field(out, "required_invalidations", std::to_string(g.required_invalidations.size()));
  out.push_back('\n');
  for (const InvalidationRecord& inv : g.required_invalidations) {
    out.append("  ");
    out.append(render_invalidation(inv));
    out.push_back('\n');
  }
  out.append("  rationale=");
  out.append(g.rationale);
  return out;
}

std::string render_publication(const PublicationReceipt& r) {
  std::string out;
  out.reserve(512);
  out.append("publication ");
  out.append(r.publication.to_string());
  out.append(" state=");
  out.append(to_token(r.state));
  out.append(" reason=");
  out.append(status_code_name(r.reason));
  out.push_back('\n');
  out.append("  ");
  append_context(out, r.context);
  out.push_back('\n');
  field(out, " version", r.version.to_string());
  field(out, "prior_version", r.prior_version.to_string());
  field(out, "writer", r.writer.to_string());
  field(out, "writer_boot", r.writer_boot.to_string());
  out.push_back('\n');
  field(out, " durability", to_token(r.durability));
  field(out, "durable", r.durable ? "true" : "false");
  field(out, "idempotent_replay", r.idempotent_replay ? "true" : "false");
  field(out, "completed_by_recovery", r.completed_by_recovery ? "true" : "false");
  out.push_back('\n');
  field(out, " content", r.content.to_string());
  out.push_back('\n');
  out.append("  rationale=");
  out.append(r.rationale);
  return out;
}

std::string render_invalidation_outcome(const InvalidationOutcome& o) {
  std::string out;
  out.reserve(384);
  out.append("invalidation_outcome ");
  out.append(o.invalidation.to_string());
  out.append(" state=");
  out.append(to_token(o.state));
  out.append(" reason=");
  out.append(status_code_name(o.reason));
  out.push_back('\n');
  out.append("  ");
  append_context(out, o.context);
  out.push_back('\n');
  field(out, " region", o.region.to_string());
  field(out, "region_generation", o.region_generation.to_string());
  field(out, "replica_generation", o.replica_generation.to_string());
  field(out, "idempotent_replay", o.idempotent_replay ? "true" : "false");
  out.push_back('\n');
  out.append("  rationale=");
  out.append(o.rationale);
  return out;
}

std::string render_sync_outcome(const SyncOutcome& o) {
  std::string out;
  out.reserve(640);
  out.append("sync_outcome ");
  out.append(o.operation.to_string());
  out.append(" state=");
  out.append(to_token(o.state));
  out.append(" reason=");
  out.append(status_code_name(o.reason));
  out.push_back('\n');
  out.append("  ");
  append_context(out, o.context);
  out.push_back('\n');
  out.append("  ");
  out.append(render_sync(o.plan));
  out.push_back('\n');
  field(out, " idempotent_replay", o.idempotent_replay ? "true" : "false");
  out.push_back('\n');
  out.append("  rationale=");
  out.append(o.rationale);
  return out;
}

} // namespace coherence
