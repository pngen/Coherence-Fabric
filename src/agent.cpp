// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/agent.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "coherence/codec.hpp"
#include "coherence/version.hpp"
#include "control_codec.hpp"

namespace coherence {
namespace {

std::int64_t parse_i64(const std::string& text, bool& ok) {
  ok = false;
  if (text.empty()) return 0;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  ok = true;
  return value;
}

std::string line(std::string_view key, std::string_view value) {
  std::string out(key);
  out.push_back('=');
  out.append(value);
  return out;
}

} // namespace

std::vector<std::string> split_command(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

Agent::Agent() = default;
Agent::~Agent() { (void)leave(); }

Status Agent::configure(AgentOptions options) {
  options_ = std::move(options);
  if (options_.object_length == 0) {
    return Status(StatusCode::InvalidArgument, "an object length is required");
  }
  for (const RegionSpec& spec : options_.regions) {
    if (spec.length == 0) {
      return Status(StatusCode::InvalidArgument, "a region length is required", spec.name);
    }
    if (!checked_range(spec.offset, spec.length, options_.object_length)) {
      return Status(StatusCode::InvalidArgument,
                    "a region extent lies outside the object extent", spec.name);
    }
  }
  if (!options_.staging_segment.empty()) {
    auto staging = adapters::SharedSegment::open(options_.staging_segment, options_.staging_length,
                                                 options_.staging_create);
    if (!staging.has_value()) return staging.status();
    staging_ = std::make_shared<adapters::SharedSegment>(std::move(staging.value()));
  }
  return Status::success();
}

RegionId Agent::next_region_identity() {
  const RequestId fresh = generate_request_id();
  return RegionId::from_value(fresh.defined() ? fresh.value() : next_region_fallback_++);
}

AuthorityContext Agent::make_context() const {
  AuthorityContext context;
  context.epoch = epoch_;
  context.participant = client_.participant();
  context.boot = options_.client.boot;
  context.object = object_;
  context.object_generation = object_generation_;
  context.policy_generation = policy_generation_;
  context.request = generate_request_id();
  return context;
}

Status Agent::join() {
  Status connected = client_.connect_and_handshake(options_.client);
  if (!connected.ok()) {
    last_error_ = connected.to_string();
    return connected;
  }
  epoch_ = client_.epoch();
  Status ensured = ensure_object();
  if (!ensured.ok()) {
    last_error_ = ensured.to_string();
    return ensured;
  }
  Status registered = register_regions();
  if (!registered.ok()) {
    last_error_ = registered.to_string();
    return registered;
  }
  joined_ = true;
  return Status::success();
}

Status Agent::refresh_object() {
  ByteWriter body;
  body.strong_id(object_);
  auto exchange = client_.request(MessageType::Query, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  if (envelope.code != StatusCode::Ok) {
    return Status(envelope.code, envelope.message, envelope.detail);
  }
  ByteReader reader(exchange.value().payload);
  std::string text;
  if (!reader.text(text, 1u << 20)) {
    return Status(StatusCode::ProtocolViolation, "object query payload could not be decoded");
  }
  auto extract = [&text](std::string_view key) -> std::string {
    const std::string needle = std::string(key) + "=";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::size_t end = pos;
    while (end < text.size() && text[end] != ' ' && text[end] != '\n') ++end;
    return text.substr(pos, end - pos);
  };
  auto to_u64 = [](const std::string& value) -> std::uint64_t {
    if (value.empty()) return 0;
    return std::strtoull(value.c_str(), nullptr, 10);
  };
  domain_ = CoherenceDomainId::from_value(to_u64(extract("domain")));
  object_generation_ = ObjectGeneration::from_value(to_u64(extract("generation")));
  policy_generation_ = PolicyGeneration::from_value(to_u64(extract("policy_generation")));
  authoritative_version_ = VersionId::from_value(to_u64(extract("authoritative_version")));
  ownership_ = OwnershipGeneration::from_value(to_u64(extract("ownership_generation")));
  return Status::success();
}

Status Agent::ensure_object() {
  // Resolve the domain by name through a snapshot so that several agents share
  // one domain even when they start concurrently. The request body must be a
  // well-formed options record; the coordinator validates it.
  auto snapshot_exchange = [&]() {
    ByteWriter options_body;
    control::encode_snapshot_options(options_body, SnapshotOptions{});
    return client_.request(MessageType::Snapshot, options_body.span());
  };
  std::string domain_id_text;
  {
    auto exchange = snapshot_exchange();
    if (!exchange.has_value()) return exchange.status();
    ResponseEnvelope envelope;
    if (!decode_exchange_envelope(exchange.value(), envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    if (envelope.code != StatusCode::Ok) {
      return Status(envelope.code, envelope.message, envelope.detail);
    }
    ByteReader reader(exchange.value().payload);
    std::string text;
    if (!reader.text(text, 1u << 22)) {
      return Status(StatusCode::ProtocolViolation, "snapshot payload could not be decoded");
    }
    const std::string needle = "name=" + options_.domain_name;
    const std::size_t at = text.find(needle);
    if (at != std::string::npos) {
      const std::size_t id_at = text.rfind("domain ", at);
      if (id_at != std::string::npos) {
        const std::size_t start = id_at + 7;
        const std::size_t end = text.find(' ', start);
        domain_id_text = text.substr(start, end - start);
      }
    }
    // Locate an existing object by name inside the domain.
    const std::string object_needle = " name=" + options_.object_name + " ";
    const std::size_t object_at = text.find(object_needle);
    if (object_at != std::string::npos) {
      const std::size_t id_at = text.rfind("object id=", object_at);
      if (id_at != std::string::npos) {
        const std::size_t start = id_at + 10;
        const std::size_t end = text.find(' ', start);
        object_ = ObjectId::from_value(std::strtoull(
            text.substr(start, end - start).c_str(), nullptr, 10));
      }
    }
  }

  if (!object_.defined()) {
    if (domain_id_text.empty()) {
      ByteWriter body;
      control::encode_create_domain(body, options_.domain_name);
      auto created = client_.request(MessageType::CreateDomain, body.span());
      if (!created.has_value()) return created.status();
      ResponseEnvelope envelope;
      if (!decode_exchange_envelope(created.value(), envelope)) {
        return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
      }
      if (envelope.code != StatusCode::Ok) {
        return Status(envelope.code, envelope.message, envelope.detail);
      }
      ByteReader reader(created.value().payload);
      DomainRecord record;
      if (!decode_domain(reader, record, default_decode_limits())) {
        return Status(StatusCode::ProtocolViolation, "domain payload could not be decoded");
      }
      domain_ = record.id;
      domain_id_text = record.id.to_string();
    }
    CoherencePolicy policy = make_policy(options_.consistency, "agent-policy");
    if (policy.consistency == ConsistencyModel::Eventual) {
      policy = eventual_policy(options_.stale_read_bound == 0 ? 8 : options_.stale_read_bound,
                               "agent-policy");
    }
    policy.stale_read_policy = options_.stale_read_policy;
    if (policy.stale_read_policy == StaleReadPolicy::Never &&
        policy.consistency == ConsistencyModel::Eventual) {
      policy.stale_read_policy = StaleReadPolicy::Bounded;
    }
    ByteWriter body;
    control::encode_register_object(
        body, CoherenceDomainId::from_value(std::strtoull(domain_id_text.c_str(), nullptr, 10)),
        options_.object_name, options_.object_length, policy);
    auto registered = client_.request(MessageType::RegisterObject, body.span());
    if (!registered.has_value()) return registered.status();
    ResponseEnvelope envelope;
    if (!decode_exchange_envelope(registered.value(), envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    if (envelope.code != StatusCode::Ok) {
      return Status(envelope.code, envelope.message, envelope.detail);
    }
    ByteReader reader(registered.value().payload);
    ObjectRecord record;
    if (!decode_object(reader, record, default_decode_limits())) {
      return Status(StatusCode::ProtocolViolation, "object payload could not be decoded");
    }
    object_ = record.id;
    domain_ = record.domain;
    object_generation_ = record.generation;
    policy_generation_ = record.policy_generation;
  }
  if (!domain_.defined() && !domain_id_text.empty()) {
    domain_ = CoherenceDomainId::from_value(std::strtoull(domain_id_text.c_str(), nullptr, 10));
  }
  return refresh_object();
}

Status Agent::send_region_registration(const RegionSpec& spec, RegionId id, std::string& rendered) {
  const adapters::StoredRegion* stored = store_.find(id);
  if (stored == nullptr) {
    return Status(StatusCode::UnknownRegion, "the local region is missing", spec.name);
  }
  RegionRegistration registration;
  registration.context = make_context();
  registration.domain = domain_;
  registration.requested_id = id;
  registration.name = spec.name;
  registration.memory_domain = stored->memory_domain;
  registration.evidence_class = stored->evidence_class;
  registration.offset = spec.offset;
  registration.length = spec.length;
  registration.address_hint = stored->address_hint;
  registration.declared_writable = spec.declared_writable;
  registration.content = fingerprint_bytes(stored->bytes());

  ByteWriter body;
  encode_region_registration(body, registration);
  auto exchange = client_.request(MessageType::RegisterRegion, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  if (envelope.code != StatusCode::Ok) {
    return Status(envelope.code, envelope.message, envelope.detail);
  }
  ByteReader reader(exchange.value().payload);
  RegionRecord record;
  if (!decode_region(reader, record, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "region payload could not be decoded");
  }
  region_generations_[id] = record.generation;
  region_versions_[id] = record.version;
  rendered = render_region(record);
  return Status::success();
}

Status Agent::register_regions() {
  for (const RegionSpec& spec : options_.regions) {
    if (region_ids_.count(spec.name) != 0) continue;
    // A process-unique region identity keeps a restarted incarnation's fresh
    // mappings distinct from the mappings its predecessor held.
    const RegionId requested = next_region_identity();
    Result<RegionId> created = [&]() -> Result<RegionId> {
      switch (spec.backing) {
        case adapters::RegionBacking::SharedMapping:
          return store_.add_shared_region(spec.name, spec.segment_name, spec.length,
                                          spec.create_segment);
        case adapters::RegionBacking::Synthetic:
          return store_.add_synthetic_cxl_region(spec.name, spec.length, spec.seed);
        case adapters::RegionBacking::HostHeap:
        case adapters::RegionBacking::DeviceLocal:
        default:
          return store_.add_host_region(spec.name, spec.length, spec.memory_domain, spec.seed,
                                        requested);
      }
    }();
    if (!created.has_value()) return created.status();
    std::string rendered;
    Status registered = send_region_registration(spec, created.value(), rendered);
    if (!registered.ok()) return registered;
    region_ids_[spec.name] = created.value();
    last_region_ = created.value();
  }
  return Status::success();
}

RegionId Agent::region_id(std::string_view name) const {
  const auto it = region_ids_.find(std::string(name));
  return it == region_ids_.end() ? RegionId::nil() : it->second;
}

RegionGeneration Agent::region_generation(RegionId id) const {
  const auto it = region_generations_.find(id);
  return it == region_generations_.end() ? RegionGeneration::nil() : it->second;
}

VersionId Agent::region_version(RegionId id) const {
  const auto it = region_versions_.find(id);
  return it == region_versions_.end() ? VersionId::nil() : it->second;
}

Status Agent::command_ack_pending(std::vector<std::string>& sink) {
  auto exchange = client_.request(MessageType::ListInvalidations, {});
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  if (envelope.code != StatusCode::Ok) {
    return Status(envelope.code, envelope.message, envelope.detail);
  }
  ByteReader reader(exchange.value().payload);
  std::uint64_t count = 0;
  if (!reader.u64(count)) return reader.status();
  if (count > 100000) {
    return Status(StatusCode::OversizedInput, "the invalidation list is implausibly large");
  }
  sink.push_back(line("pending_invalidations", std::to_string(count)));
  for (std::uint64_t i = 0; i < count; ++i) {
    InvalidationRecord record;
    if (!decode_invalidation(reader, record, default_decode_limits())) {
      return reader.status();
    }
    InvalidationAck acknowledgement;
    acknowledgement.context = make_context();
    acknowledgement.invalidation = record.id;
    acknowledgement.region = record.region;
    acknowledgement.region_generation = record.region_generation;
    acknowledgement.replica_generation = record.replica_generation;
    acknowledgement.superseded_version = record.superseded_version;
    ByteWriter body;
    encode_invalidation_ack(body, acknowledgement);
    auto acked = client_.request(MessageType::AcknowledgeInvalidation, body.span());
    if (!acked.has_value()) return acked.status();
    ResponseEnvelope ack_envelope;
    if (!decode_exchange_envelope(acked.value(), ack_envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    sink.push_back("ack invalidation=" + record.id.to_string() +
                   " status=" + std::string(status_code_name(ack_envelope.code)));
    if (ack_envelope.code == StatusCode::Ok) {
      const auto it = region_versions_.find(record.region);
      if (it != region_versions_.end()) {
        it->second = record.superseded_version;
      }
    }
  }
  return Status::success();
}

Status Agent::command_read(const std::vector<std::string>& args, std::vector<std::string>& sink) {
  ReadRequest request;
  request.context = make_context();
  RegionId region = args.size() > 1 ? region_id(args[1]) : RegionId::nil();
  if (args.size() > 1 && !region.defined()) {
    return Status(StatusCode::UnknownRegion, "no such local region", args[1]);
  }
  request.region = region;
  request.region_generation = region.defined() ? region_generation(region)
                                               : RegionGeneration::nil();
  request.require_current = true;
  request.snapshot_version = authoritative_version_;
  ByteWriter body;
  encode_read_request(body, request);
  auto exchange = client_.request(MessageType::AcquireRead, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  ByteReader reader(exchange.value().payload);
  ReadDecision decision;
  if (!decode_read_decision(reader, decision, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "read decision could not be decoded");
  }
  last_region_ = decision.region.defined() ? decision.region : last_region_;
  sink.push_back(line("read_outcome", to_token(decision.outcome)));
  sink.push_back(line("read_reason", status_code_name(decision.reason)));
  sink.push_back(line("read_replica", decision.region.to_string()));
  sink.push_back(line("read_replica_state", to_token(decision.region_state)));
  sink.push_back(line("read_replica_version", decision.region_version.to_string()));
  sink.push_back(line("read_authoritative_version", decision.authoritative_version.to_string()));
  sink.push_back(line("read_sync_required", decision.sync_required ? "true" : "false"));
  sink.push_back(line("read_stale_allowed", decision.stale_allowed ? "true" : "false"));
  sink.push_back(line("read_evidence_fresh", decision.evidence_fresh ? "true" : "false"));
  if (decision.lease.defined()) {
    last_lease_ = decision.lease;
    sink.push_back(line("read_lease", decision.lease.to_string()));
  }
  return Status::success();
}

Status Agent::command_write(const std::vector<std::string>& args, std::vector<std::string>& sink) {
  WriteRequest request;
  request.context = make_context();
  RegionId region = args.size() > 1 ? region_id(args[1]) : last_region_;
  if (args.size() > 1 && !region.defined()) {
    return Status(StatusCode::UnknownRegion, "no such local region", args[1]);
  }
  request.region = region;
  request.region_generation = region_generation(region);
  ByteWriter body;
  encode_write_request(body, request);
  auto exchange = client_.request(MessageType::AcquireWrite, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  ByteReader reader(exchange.value().payload);
  WriteGrant grant;
  if (!decode_write_grant(reader, grant, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "write grant could not be decoded");
  }
  ownership_ = grant.context.ownership_generation;
  write_held_ = grant.granted;
  sink.push_back(line("write_granted", grant.granted ? "true" : "false"));
  sink.push_back(line("write_reason", status_code_name(grant.reason)));
  sink.push_back(line("write_may_mutate_now", grant.may_mutate_now ? "true" : "false"));
  sink.push_back(line("write_ownership_generation", ownership_.to_string()));
  sink.push_back(line("write_base_version", grant.base_version.to_string()));
  for (const InvalidationRecord& invalidation : grant.required_invalidations) {
    sink.push_back("required_invalidation id=" + invalidation.id.to_string() + " region=" +
                   invalidation.region.to_string() + " target=" +
                   invalidation.target_participant.to_string());
  }
  return Status::success();
}

Status Agent::command_publish(const std::vector<std::string>& args, std::vector<std::string>& sink) {
  RegionId region = args.size() > 1 ? region_id(args[1]) : last_region_;
  if (!region.defined()) {
    return Status(StatusCode::UnknownRegion, "no local region was selected");
  }
  VersionId base = authoritative_version_;
  if (args.size() > 2) {
    bool ok = false;
    const std::int64_t parsed = parse_i64(args[2], ok);
    if (!ok || parsed < 0) {
      return Status(StatusCode::InvalidArgument, "the base version must be a non-negative integer");
    }
    base = VersionId::from_value(static_cast<std::uint64_t>(parsed));
  }
  adapters::StoredRegion* stored = store_.find(region);
  if (stored == nullptr) {
    return Status(StatusCode::UnknownRegion, "the local region is missing");
  }
  const ContentFingerprint content = fingerprint_bytes(stored->bytes());
  const RegionSpec* spec = nullptr;
  for (const RegionSpec& candidate : options_.regions) {
    if (region_ids_.count(candidate.name) != 0 && region_ids_.at(candidate.name) == region) {
      spec = &candidate;
      break;
    }
  }
  // Publish the authoritative bytes into the shared staging segment first. This
  // is a real cross-process byte path, not a simulation.
  if (staging_ != nullptr && spec != nullptr) {
    if (!checked_range(spec->offset, spec->length, staging_->length())) {
      return Status(StatusCode::InvalidArgument,
                    "the region extent lies outside the staging segment");
    }
    std::memcpy(staging_->data() + spec->offset, stored->bytes().data(),
                static_cast<std::size_t>(spec->length));
  }

  PublishRequest request;
  request.context = make_context();
  request.ownership_generation = ownership_;
  request.region = region;
  request.region_generation = region_generation(region);
  request.expected_base_version = base;
  // With no prior authoritative version this is the initial establishment,
  // which the engine only permits through its explicit path.
  request.allow_without_dirty = !base.defined();
  request.content = content;
  ByteWriter body;
  encode_publish_request(body, request);
  auto exchange = client_.request(MessageType::Publish, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  ByteReader reader(exchange.value().payload);
  PublicationReceipt receipt;
  if (!decode_publication(reader, receipt, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "publication receipt could not be decoded");
  }
  sink.push_back(line("publish_state", to_token(receipt.state)));
  sink.push_back(line("publish_reason", status_code_name(receipt.reason)));
  sink.push_back(line("publish_version", receipt.version.to_string()));
  sink.push_back(line("publish_prior_version", receipt.prior_version.to_string()));
  sink.push_back(line("publish_durable", receipt.durable ? "true" : "false"));
  sink.push_back(line("publish_idempotent_replay", receipt.idempotent_replay ? "true" : "false"));
  if (receipt.state == PublicationState::Committed) {
    authoritative_version_ = receipt.version;
    region_versions_[region] = receipt.version;
  }
  return Status::success();
}

Status Agent::command_sync_begin(const std::vector<std::string>& args,
                                 std::vector<std::string>& sink) {
  if (args.size() < 2) {
    return Status(StatusCode::InvalidArgument, "sync-begin requires a destination region");
  }
  const RegionId destination = region_id(args[1]);
  if (!destination.defined()) {
    return Status(StatusCode::UnknownRegion, "no such local region", args[1]);
  }
  RegionId source;
  if (args.size() > 2 && args[2] != "auto") {
    // A source may be named locally, or addressed by its explicit identity when
    // it belongs to another participant.
    source = region_id(args[2]);
    bool parsed_ok = false;
    const std::int64_t numeric = parse_i64(args[2], parsed_ok);
    if (!source.defined() && parsed_ok && numeric > 0) {
      source = RegionId::from_value(static_cast<std::uint64_t>(numeric));
    }
    if (!source.defined()) {
      return Status(StatusCode::UnknownRegion,
                    "the source is neither a local region name nor a region identity", args[2]);
    }
  }
  SyncRequest request;
  request.context = make_context();
  request.kind = SyncOperationKind::Copy;
  request.source_region = source;
  request.source_region_generation = source.defined() ? region_generation(source)
                                                      : RegionGeneration::nil();
  request.destination_region = destination;
  request.destination_region_generation = region_generation(destination);
  request.ownership_generation = ownership_;
  request.transport = staging_ != nullptr ? "shared_mapping_staging" : "delegated";
  ByteWriter body;
  encode_sync_request(body, request);
  auto exchange = client_.request(MessageType::SyncBegin, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  ByteReader reader(exchange.value().payload);
  SyncOutcome outcome;
  if (!decode_sync_outcome(reader, outcome, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "synchronization outcome could not be decoded");
  }
  sink.push_back(line("sync_operation", outcome.operation.to_string()));
  sink.push_back(line("sync_state", to_token(outcome.state)));
  sink.push_back(line("sync_reason", status_code_name(outcome.reason)));
  sink.push_back(line("sync_source_region", outcome.plan.source_region.to_string()));
  region_versions_[outcome.plan.source_region] = outcome.plan.source_version;
  region_generations_[outcome.plan.source_region] = outcome.plan.source_region_generation;
  sink.push_back(line("sync_source_version", outcome.plan.source_version.to_string()));
  sink.push_back(line("sync_expected_content", outcome.plan.expected_content.to_string()));
  sink.push_back(line("sync_transport", outcome.plan.transport));
  sink.push_back(line("sync_extent_offset", std::to_string(outcome.plan.extent_offset)));
  sink.push_back(line("sync_extent_length", std::to_string(outcome.plan.extent_length)));
  sync_plans_[outcome.operation] = outcome.plan;
  return Status::success();
}

Status Agent::command_sync_complete(const std::vector<std::string>& args,
                                    std::vector<std::string>& sink) {
  if (args.size() < 2) {
    return Status(StatusCode::InvalidArgument, "sync-complete requires a destination region");
  }
  const RegionId destination = region_id(args[1]);
  if (!destination.defined()) {
    return Status(StatusCode::UnknownRegion, "no such local region", args[1]);
  }
  if (args.size() < 3) {
    return Status(StatusCode::InvalidArgument, "sync-complete requires a synchronization identity");
  }
  const SyncOperationId operation =
      SyncOperationId::from_value(std::strtoull(args[2].c_str(), nullptr, 10));
  adapters::StoredRegion* stored = store_.find(destination);
  if (stored == nullptr) {
    return Status(StatusCode::UnknownRegion, "the local destination region is missing");
  }
  const RegionSpec* spec = nullptr;
  for (const RegionSpec& candidate : options_.regions) {
    if (region_ids_.count(candidate.name) != 0 && region_ids_.at(candidate.name) == destination) {
      spec = &candidate;
      break;
    }
  }
  (void)spec;
  if (staging_ == nullptr) {
    return Status(StatusCode::Unsupported,
                  "no staging segment is configured, so no real bytes can be moved");
  }
  const auto plan = sync_plans_.find(operation);
  if (plan == sync_plans_.end()) {
    return Status(StatusCode::UnknownSyncOperation,
                  "the agent has no plan for this synchronization identity");
  }
  if (plan->second.extent_length != stored->length) {
    return Status(StatusCode::InvalidArgument,
                  "the planned extent does not match the destination replica length");
  }
  if (!checked_range(plan->second.extent_offset, plan->second.extent_length, staging_->length())) {
    return Status(StatusCode::InvalidArgument,
                  "the planned extent lies outside the staging segment");
  }
  // Real byte movement: copy out of the shared mapping into this process's own
  // replica, then fingerprint what actually arrived.
  std::memcpy(stored->bytes().data(), staging_->data() + plan->second.extent_offset,
              static_cast<std::size_t>(plan->second.extent_length));
  const ContentFingerprint observed = fingerprint_bytes(stored->bytes());

  SyncCompleteRequest request;
  request.context = make_context();
  request.operation = operation;
  request.destination_region = destination;
  request.destination_region_generation = region_generation(destination);
  const auto planned = sync_plans_.find(operation);
  if (planned == sync_plans_.end()) {
    return Status(StatusCode::UnknownSyncOperation,
                  "the agent has no plan for this synchronization identity");
  }
  request.destination_new_version = planned->second.source_version;
  request.observed_content = observed;
  request.content_moved = true;
  ByteWriter body;
  encode_sync_complete(body, request);
  auto exchange = client_.request(MessageType::SyncComplete, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  ByteReader reader(exchange.value().payload);
  SyncOutcome outcome;
  if (!decode_sync_outcome(reader, outcome, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "synchronization outcome could not be decoded");
  }
  sink.push_back(line("sync_state", to_token(outcome.state)));
  sink.push_back(line("sync_reason", status_code_name(outcome.reason)));
  sink.push_back(line("sync_observed_content", observed.to_string()));
  if (outcome.state == SyncState::Completed) {
    region_versions_[destination] = outcome.plan.source_version;
    authoritative_version_ = outcome.plan.source_version;
  }
  return Status::success();
}

Status Agent::command_invalidate(const std::vector<std::string>& args,
                                 std::vector<std::string>& sink) {
  if (args.size() < 2) {
    return Status(StatusCode::InvalidArgument, "invalidate requires a destination region");
  }
  const RegionId region = region_id(args[1]);
  if (!region.defined()) {
    return Status(StatusCode::UnknownRegion, "no such local region", args[1]);
  }
  InvalidationRequest request;
  request.context.object = object_;
  request.context.object_generation = object_generation_;
  request.context.epoch = epoch_;
  request.context.policy_generation = policy_generation_;
  request.target_region = region;
  request.target_region_generation = region_generation(region);
  request.target_replica_generation = ReplicaGeneration::from_value(1);
  request.ownership_generation = ownership_;
  request.published_version = authoritative_version_;
  ByteWriter body;
  encode_invalidation_request(body, request);
  auto exchange = client_.request(MessageType::Invalidate, body.span());
  if (!exchange.has_value()) return exchange.status();
  ResponseEnvelope envelope;
  if (!decode_exchange_envelope(exchange.value(), envelope)) {
    return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
  }
  ByteReader reader(exchange.value().payload);
  InvalidationOutcome outcome;
  if (!decode_invalidation_outcome(reader, outcome, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "invalidation outcome could not be decoded");
  }
  sink.push_back(line("invalidate_id", outcome.invalidation.to_string()));
  sink.push_back(line("invalidate_state", to_token(outcome.state)));
  sink.push_back(line("invalidate_reason", status_code_name(outcome.reason)));
  return Status::success();
}

Status Agent::execute(std::string_view command, std::vector<std::string>& sink) {
  const std::vector<std::string> args = split_command(command);
  if (args.empty()) return Status::success();
  const std::string& verb = args[0];

  if (verb == "echo") {
    sink.push_back(std::string(command.substr(command.find(' ') + 1)));
    return Status::success();
  }
  if (verb == "refresh") return refresh_object();
  if (verb == "ack-pending") return command_ack_pending(sink);
  if (verb == "read") return command_read(args, sink);
  if (verb == "write") return command_write(args, sink);
  if (verb == "publish") return command_publish(args, sink);
  if (verb == "sync-begin") return command_sync_begin(args, sink);
  if (verb == "sync-complete") return command_sync_complete(args, sink);
  if (verb == "invalidate") return command_invalidate(args, sink);

  if (verb == "fill") {
    if (args.size() < 3) {
      return Status(StatusCode::InvalidArgument, "fill requires a region and a seed");
    }
    const RegionId region = region_id(args[1]);
    if (!region.defined()) {
      return Status(StatusCode::UnknownRegion, "no such local region", args[1]);
    }
    bool ok = false;
    const std::int64_t seed = parse_i64(args[2], ok);
    if (!ok) return Status(StatusCode::InvalidArgument, "the seed must be an integer");
    Status filled = store_.fill(region, static_cast<std::uint64_t>(seed));
    if (!filled.ok()) return filled;
    sink.push_back(line("fill_region", args[1]));
    const auto fp = store_.fingerprint(region);
    if (fp.has_value()) sink.push_back(line("local_content", fp.value().to_string()));
    return Status::success();
  }
  if (verb == "poke") {
    if (args.size() < 4) {
      return Status(StatusCode::InvalidArgument, "poke requires region, offset and value");
    }
    const RegionId region = region_id(args[1]);
    if (!region.defined()) {
      return Status(StatusCode::UnknownRegion, "no such local region", args[1]);
    }
    bool ok = false;
    const std::int64_t offset = parse_i64(args[2], ok);
    if (!ok || offset < 0) return Status(StatusCode::InvalidArgument, "bad offset");
    const std::int64_t value = parse_i64(args[3], ok);
    if (!ok || value < 0 || value > 255) return Status(StatusCode::InvalidArgument, "bad value");
    Status poked = store_.poke(region, static_cast<std::uint64_t>(offset),
                               static_cast<std::uint8_t>(value));
    if (!poked.ok()) return poked;
    const auto fp = store_.fingerprint(region);
    if (fp.has_value()) sink.push_back(line("local_content", fp.value().to_string()));
    return Status::success();
  }
  if (verb == "mark-dirty") {
    const RegionId region = args.size() > 1 ? region_id(args[1]) : last_region_;
    if (!region.defined()) {
      return Status(StatusCode::UnknownRegion, "no local region was selected");
    }
    DirtyRequest request;
    request.context = make_context();
    request.region = region;
    request.region_generation = region_generation(region);
    request.ownership_generation = ownership_;
    request.base_version = authoritative_version_;
    const adapters::StoredRegion* stored = store_.find(region);
    if (stored != nullptr) request.content = fingerprint_bytes(stored->bytes());
    ByteWriter body;
    encode_dirty_request(body, request);
    auto exchange = client_.request(MessageType::MarkDirty, body.span());
    if (!exchange.has_value()) return exchange.status();
    ResponseEnvelope envelope;
    if (!decode_exchange_envelope(exchange.value(), envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    sink.push_back(line("mark_dirty_status", status_code_name(envelope.code)));
    return Status::success();
  }
  if (verb == "revalidate") {
    const RegionId region = args.size() > 1 ? region_id(args[1]) : last_region_;
    if (!region.defined()) {
      return Status(StatusCode::UnknownRegion, "no local region was selected");
    }
    RevalidateRequest request;
    request.context = make_context();
    request.region = region;
    request.region_generation = region_generation(region);
    request.observed_version = authoritative_version_.defined() ? authoritative_version_
                                                                : VersionId::from_value(1);
    if (args.size() > 2) {
      bool ok = false;
      const std::int64_t parsed = parse_i64(args[2], ok);
      if (!ok || parsed < 0) {
        return Status(StatusCode::InvalidArgument, "bad observed version");
      }
      request.observed_version = VersionId::from_value(static_cast<std::uint64_t>(parsed));
    }
    const adapters::StoredRegion* stored = store_.find(region);
    if (stored != nullptr) request.content = fingerprint_bytes(stored->bytes());
    request.byte_compared = true;
    ByteWriter body;
    encode_revalidate_request(body, request);
    auto exchange = client_.request(MessageType::Revalidate, body.span());
    if (!exchange.has_value()) return exchange.status();
    ResponseEnvelope envelope;
    if (!decode_exchange_envelope(exchange.value(), envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    sink.push_back(line("revalidate_status", status_code_name(envelope.code)));
    ByteReader reader(exchange.value().payload);
    RegionRecord record;
    if (decode_region(reader, record, default_decode_limits())) {
      sink.push_back(line("revalidate_state", to_token(record.state)));
      sink.push_back(line("revalidate_version", record.version.to_string()));
      region_versions_[region] = record.version;
      region_generations_[region] = record.generation;
    }
    return Status::success();
  }
  if (verb == "release-write") {
    ReleaseRequest request;
    request.context = make_context();
    request.ownership_generation = ownership_;
    request.release_write_authority = true;
    ByteWriter body;
    encode_release_request(body, request);
    auto exchange = client_.request(MessageType::Release, body.span());
    if (!exchange.has_value()) return exchange.status();
    ResponseEnvelope envelope;
    if (!decode_exchange_envelope(exchange.value(), envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    sink.push_back(line("release_status", status_code_name(envelope.code)));
    write_held_ = false;
    return Status::success();
  }
  if (verb == "release-read") {
    ReleaseRequest request;
    request.context = make_context();
    request.lease = last_lease_;
    ByteWriter body;
    encode_release_request(body, request);
    auto exchange = client_.request(MessageType::Release, body.span());
    if (!exchange.has_value()) return exchange.status();
    ResponseEnvelope envelope;
    if (!decode_exchange_envelope(exchange.value(), envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    sink.push_back(line("release_status", status_code_name(envelope.code)));
    return Status::success();
  }
  if (verb == "audit" || verb == "snapshot" || verb == "participants" || verb == "objects" ||
      verb == "regions" || verb == "decisions") {
    MessageType type = MessageType::Snapshot;
    if (verb == "audit") type = MessageType::Audit;
    if (verb == "participants") type = MessageType::ListParticipants;
    if (verb == "objects") type = MessageType::ListObjects;
    if (verb == "regions") type = MessageType::ListRegions;
    if (verb == "decisions") type = MessageType::DecisionLog;
    ByteWriter body;
    if (type == MessageType::Snapshot) {
      control::encode_snapshot_options(body, SnapshotOptions{});
    }
    auto exchange = client_.request(type, body.span());
    if (!exchange.has_value()) return exchange.status();
    ResponseEnvelope envelope;
    if (!decode_exchange_envelope(exchange.value(), envelope)) {
      return Status(StatusCode::ProtocolViolation, "response envelope could not be decoded");
    }
    ByteReader reader(exchange.value().payload);
    std::string text;
    if (!reader.text(text, 1u << 22)) {
      return Status(StatusCode::ProtocolViolation, "text payload could not be decoded");
    }
    sink.push_back(text);
    return Status::success();
  }
  if (verb == "noop") return Status::success();

  last_error_ = "unknown command: " + verb;
  return Status(StatusCode::InvalidArgument, "unknown agent command", verb);
}

Status Agent::run_script(std::vector<std::string>& sink) {
  for (const std::string& command : options_.script) {
    sink.push_back("command=" + command);
    Status executed = execute(command, sink);
    if (!executed.ok()) {
      sink.push_back("command_failed=" + executed.to_string());
      last_error_ = executed.to_string();
      return executed;
    }
  }
  return Status::success();
}

Status Agent::leave() {
  if (!joined_) {
    (void)client_.close();
    return Status::success();
  }
  if (write_held_) {
    ReleaseRequest request;
    request.context = make_context();
    request.ownership_generation = ownership_;
    request.release_write_authority = true;
    ByteWriter body;
    encode_release_request(body, request);
    (void)client_.request(MessageType::Release, body.span());
    write_held_ = false;
  }
  joined_ = false;
  return client_.close();
}

} // namespace coherence
