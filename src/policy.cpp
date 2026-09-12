// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/policy.hpp"

#include <string>

namespace coherence {
namespace {

StatusCode validate(const CoherencePolicy& p, std::string* why) {
  if (p.consistency == ConsistencyModel::Unspecified) {
    *why = "consistency model is unspecified";
    return StatusCode::InvalidArgument;
  }
  if (p.write_ownership == WriteOwnershipMode::Unspecified) {
    *why = "write ownership mode is unspecified";
    return StatusCode::InvalidArgument;
  }
  if (p.publication_durability == PublicationDurability::Unspecified) {
    *why = "publication durability is unspecified";
    return StatusCode::InvalidArgument;
  }
  if (p.conflict_behavior == ConflictBehavior::Unspecified) {
    *why = "conflict behavior is unspecified";
    return StatusCode::InvalidArgument;
  }
  if (p.recovery_policy == RecoveryPolicy::Unspecified) {
    *why = "recovery policy is unspecified";
    return StatusCode::InvalidArgument;
  }
  if (p.stale_read_policy == StaleReadPolicy::Unspecified) {
    *why = "stale read policy is unspecified";
    return StatusCode::InvalidArgument;
  }
  if (p.dirty_loss_policy == DirtyLossPolicy::Unspecified) {
    *why = "dirty loss policy is unspecified";
    return StatusCode::InvalidArgument;
  }

  // Declared-but-unimplemented semantics fail closed.
  if (p.write_ownership == WriteOwnershipMode::MultiWriterUnsupported) {
    *why =
        "multi-writer semantics are not implemented; two writers must never "
        "coexist for one object generation";
    return StatusCode::Unsupported;
  }

  if (p.stale_read_policy == StaleReadPolicy::Bounded && p.stale_read_bound_versions == 0) {
    *why = "bounded stale reads require a non-zero stale_read_bound_versions";
    return StatusCode::InvalidArgument;
  }

  if (p.stale_read_policy == StaleReadPolicy::Never && p.consistency == ConsistencyModel::Eventual) {
    *why =
        "eventual consistency with stale reads forbidden is contradictory; reads "
        "could never succeed";
    return StatusCode::InvalidArgument;
  }

  if (p.allowed_domains_mask == 0) {
    *why = "allowed_domains_mask permits no memory domain";
    return StatusCode::InvalidArgument;
  }

  if (p.consistency == ConsistencyModel::Strict &&
      p.stale_read_policy != StaleReadPolicy::Never) {
    *why = "strict consistency requires stale_read_policy == never";
    return StatusCode::InvalidArgument;
  }

  if (p.require_content_fingerprint == false &&
      p.publication_durability == PublicationDurability::DurableMetadata &&
      p.consistency == ConsistencyModel::Strict) {
    *why =
        "strict durable publication requires a content fingerprint so that "
        "synchronization can be verified";
    return StatusCode::InvalidArgument;
  }

  return StatusCode::Ok;
}

} // namespace

Status validate_policy(const CoherencePolicy& policy) {
  std::string why;
  const StatusCode code = validate(policy, &why);
  if (code == StatusCode::Ok) return Status::success();
  return Status(code, "policy rejected", std::move(why));
}

CoherencePolicy strict_policy(std::string name) {
  CoherencePolicy p;
  p.name = std::move(name);
  p.consistency = ConsistencyModel::Strict;
  p.write_ownership = WriteOwnershipMode::SingleWriterExclusive;
  p.publication_durability = PublicationDurability::DurableMetadata;
  p.conflict_behavior = ConflictBehavior::InvalidateReaders;
  p.recovery_policy = RecoveryPolicy::CompleteDurablePending;
  p.stale_read_policy = StaleReadPolicy::Never;
  p.stale_read_bound_versions = 0;
  p.dirty_loss_policy = DirtyLossPolicy::RequireRecovery;
  // Evidence is invalidated by events. A deployment that wants a logical
  // operation bound sets it explicitly.
  p.evidence_max_age_operations = 0;
  p.require_invalidation_acks_before_publication = true;
  p.require_sync_completion_for_currentness = true;
  p.require_content_fingerprint = true;
  p.allowed_domains_mask = 0xFFFFFFFFu;
  return p;
}

CoherencePolicy release_acquire_policy(std::string name) {
  CoherencePolicy p = strict_policy(std::move(name));
  p.consistency = ConsistencyModel::ReleaseAcquire;
  return p;
}

CoherencePolicy snapshot_policy(std::string name) {
  CoherencePolicy p = strict_policy(std::move(name));
  p.consistency = ConsistencyModel::Snapshot;
  return p;
}

CoherencePolicy eventual_policy(std::uint64_t stale_bound, std::string name) {
  CoherencePolicy p = strict_policy(std::move(name));
  p.consistency = ConsistencyModel::Eventual;
  p.stale_read_policy = StaleReadPolicy::Bounded;
  p.stale_read_bound_versions = stale_bound;
  p.recovery_policy = RecoveryPolicy::Conservative;
  return p;
}

CoherencePolicy make_policy(ConsistencyModel model, std::string name) {
  switch (model) {
    case ConsistencyModel::Strict:
      return strict_policy(name.empty() ? std::string("strict") : std::move(name));
    case ConsistencyModel::ReleaseAcquire:
      return release_acquire_policy(name.empty() ? std::string("release_acquire") : std::move(name));
    case ConsistencyModel::Snapshot:
      return snapshot_policy(name.empty() ? std::string("snapshot") : std::move(name));
    case ConsistencyModel::Eventual:
      return eventual_policy(8, name.empty() ? std::string("eventual") : std::move(name));
    case ConsistencyModel::Unspecified:
    default: {
      CoherencePolicy p;
      p.name = name.empty() ? std::string("unspecified") : std::move(name);
      return p;
    }
  }
}

std::string render_policy(const CoherencePolicy& p) {
  std::string out;
  out.reserve(512);
  out.append("policy id=");
  out.append(p.id.is_nil() ? std::string("unassigned") : p.id.to_string());
  out.append(" generation=");
  out.append(p.generation.is_nil() ? std::string("unassigned") : p.generation.to_string());
  out.append(" name=");
  out.append(p.name);
  out.push_back('\n');
  out.append("  consistency=");
  out.append(to_token(p.consistency));
  out.append(" write_ownership=");
  out.append(to_token(p.write_ownership));
  out.append(" publication_durability=");
  out.append(to_token(p.publication_durability));
  out.push_back('\n');
  out.append("  conflict_behavior=");
  out.append(to_token(p.conflict_behavior));
  out.append(" recovery_policy=");
  out.append(to_token(p.recovery_policy));
  out.append(" stale_read_policy=");
  out.append(to_token(p.stale_read_policy));
  out.push_back('\n');
  out.append("  stale_read_bound_versions=");
  out.append(std::to_string(p.stale_read_bound_versions));
  out.append(" evidence_max_age_operations=");
  out.append(std::to_string(p.evidence_max_age_operations));
  out.push_back('\n');
  out.append("  dirty_loss_policy=");
  out.append(to_token(p.dirty_loss_policy));
  out.append(" require_invalidation_acks_before_publication=");
  out.append(p.require_invalidation_acks_before_publication ? "true" : "false");
  out.push_back('\n');
  out.append("  require_sync_completion_for_currentness=");
  out.append(p.require_sync_completion_for_currentness ? "true" : "false");
  out.append(" require_content_fingerprint=");
  out.append(p.require_content_fingerprint ? "true" : "false");
  out.push_back('\n');
  out.append("  allowed_domains=");
  bool first = true;
  for (std::uint32_t raw = 0; raw <= static_cast<std::uint32_t>(MemoryDomain::Synthetic); ++raw) {
    const auto domain = static_cast<MemoryDomain>(raw);
    if (!p.allows_domain(domain)) continue;
    if (!first) out.push_back(',');
    first = false;
    out.append(to_token(domain));
  }
  return out;
}

std::string summarize_policy(const CoherencePolicy& p) {
  std::string out = p.name;
  out.push_back('/');
  out.append(p.generation.is_nil() ? std::string("-") : p.generation.to_string());
  out.append(" consistency=");
  out.append(to_token(p.consistency));
  out.append(" ownership=");
  out.append(to_token(p.write_ownership));
  out.append(" durability=");
  out.append(to_token(p.publication_durability));
  return out;
}

} // namespace coherence
