// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
// Remote command implementations for coherence_cli. Included by the app.

static void emit_block(const std::string& text) {
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      if (start < text.size()) emit(text.substr(start));
      break;
    }
    emit(text.substr(start, end - start));
    start = end + 1;
  }
}

static int report_failure(const coherence::Exchange& exchange) {
  emit_failure(Status(exchange.envelope.code, exchange.envelope.message, exchange.envelope.detail));
  return 1;
}

/// Send a request and decode the structured response payload with the library
/// codec, rendering it with the library renderer. The CLI never re-implements
/// the wire format.
template <typename Record, typename Decode, typename Render>
int exchange_record(Remote& state, coherence::MessageType type, coherence::ByteSpan body,
                    Decode decode, Render render) {
  auto exchange = state.client.request(type, body);
  if (!exchange.has_value()) {
    emit_failure(exchange.status());
    return 1;
  }
  if (exchange.value().envelope.code != StatusCode::Ok) return report_failure(exchange.value());
  coherence::ByteReader reader(exchange.value().payload);
  Record record;
  if (!decode(reader, record, coherence::default_decode_limits()) || !reader.at_end()) {
    emit("error code=protocol_violation message=response payload could not be decoded");
    return 1;
  }
  emit_block(render(record));
  return 0;
}

/// Send a request and print the textual response payload verbatim.
static int exchange_text(Remote& state, coherence::MessageType type, coherence::ByteSpan body) {
  auto exchange = state.client.request(type, body);
  if (!exchange.has_value()) {
    emit_failure(exchange.status());
    return 1;
  }
  if (exchange.value().envelope.code != StatusCode::Ok) return report_failure(exchange.value());
  coherence::ByteReader reader(exchange.value().payload);
  std::string text;
  if (!reader.text(text, 1u << 22)) {
    emit("error code=protocol_violation message=text payload could not be decoded");
    return 1;
  }
  emit_block(text);
  return 0;
}

static std::string render_domain_record(const coherence::DomainRecord& record) {
  return "domain id=" + record.id.to_string() + " name=" + record.name + " lifecycle=" +
         std::string(coherence::to_token(record.lifecycle)) +
         " created_epoch=" + record.created_epoch.to_string() +
         " current_epoch=" + record.current_epoch.to_string();
}

static std::string render_release_record(const coherence::ReleaseOutcome& outcome) {
  return std::string("release reason=") + std::string(coherence::status_code_name(outcome.reason)) +
         " lease=" + outcome.lease.to_string() +
         " already_released=" + (outcome.already_released ? "true" : "false") +
         "\n  rationale=" + outcome.rationale;
}

int run_remote(Remote& state, const Args& args) {
  const std::string group = args.positional[0];
  const std::string verb = args.positional.size() > 1 ? args.positional[1] : std::string();

  if (group == "status") {
    emit(std::string("build=") + coherence::build_identification());
    emit("epoch=" + state.client.epoch().to_string());
    emit("session=" + state.client.session().session.to_string());
    emit("participant=" + state.client.participant().to_string());
    emit(state.client.session().observer ? "mode=observer" : "mode=participant");
    const auto pong = state.client.ping(1);
    if (!pong.has_value()) {
      emit_failure(pong.status());
      return 1;
    }
    emit("ping=ok");
    return 0;
  }
  if (group == "snapshot") {
    coherence::ByteWriter body;
    coherence::SnapshotOptions options;
    options.object_filter = coherence::ObjectId::from_value(args.number("object"));
    body.u64(options.object_filter.value());
    body.u64(options.max_objects);
    body.boolean(options.include_regions);
    body.boolean(options.include_participants);
    body.boolean(options.include_invalidations);
    body.boolean(options.include_syncs);
    return exchange_text(state, coherence::MessageType::Snapshot, body.span());
  }
  if (group == "audit" || group == "verify") {
    auto exchange = state.client.request(coherence::MessageType::Audit, {});
    if (!exchange.has_value()) {
      emit_failure(exchange.status());
      return 1;
    }
    if (exchange.value().envelope.code != StatusCode::Ok) return report_failure(exchange.value());
    coherence::ByteReader reader(exchange.value().payload);
    std::string text;
    if (!reader.text(text, 1u << 22)) return 1;
    emit_block(text);
    return text.find("result=clean") != std::string::npos ? 0 : 4;
  }
  if (group == "decisions") {
    return exchange_text(state, coherence::MessageType::DecisionLog, {});
  }
  if (group == "recovery") {
    return exchange_text(state, coherence::MessageType::RecoveryInfo, {});
  }
  if (group == "participant" && (verb == "list" || verb == "show")) {
    return exchange_text(state, coherence::MessageType::ListParticipants, {});
  }
  if (group == "object" && (verb == "list" || verb == "show")) {
    return exchange_text(state, coherence::MessageType::ListObjects, {});
  }
  if (group == "region" && (verb == "list" || verb == "show")) {
    return exchange_text(state, coherence::MessageType::ListRegions, {});
  }
  if (group == "domain" && verb == "create") {
    coherence::ByteWriter body;
    body.text(args.get("name", "default"));
    return exchange_record<coherence::DomainRecord>(state, coherence::MessageType::CreateDomain,
                                                   body.span(), coherence::decode_domain,
                                                   render_domain_record);
  }
  if (group == "domain" && verb == "show") {
    return exchange_text(state, coherence::MessageType::Snapshot, {});
  }
  if (group == "object" && verb == "create") {
    coherence::ByteWriter body;
    body.strong_id(coherence::CoherenceDomainId::from_value(args.number("domain")));
    body.text(args.get("name", "object"));
    body.u64(args.number("length"));
    coherence::encode_policy(body, build_policy(args));
    return exchange_record<coherence::ObjectRecord>(state, coherence::MessageType::RegisterObject,
                                                    body.span(), coherence::decode_object,
                                                    coherence::render_object);
  }
  if (group == "object" && verb == "resolve-recovery") {
    coherence::ByteWriter body;
    body.strong_id(coherence::ObjectId::from_value(args.number("object")));
    body.u64(args.number("generation"));
    body.text(args.get("note", ""));
    return exchange_record<coherence::ObjectRecord>(
        state, coherence::MessageType::ResolveRecovery, body.span(), coherence::decode_object,
        coherence::render_object);
  }
  if (group == "object" && verb == "retire") {
    coherence::ByteWriter body;
    body.u64(args.number("object"));
    body.u64(args.number("generation"));
    return exchange_text(state, coherence::MessageType::RetireObject, body.span());
  }
  if (group == "region" && verb == "register") {
    coherence::RegionRegistration registration;
    registration.context.epoch = state.client.epoch();
    registration.context.object = coherence::ObjectId::from_value(args.number("object"));
    registration.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    registration.context.request = coherence::generate_request_id();
    registration.requested_id = coherence::RegionId::from_value(args.number("region"));
    registration.requested_replica = coherence::ReplicaId::from_value(args.number("replica"));
    registration.name = args.get("name", "replica");
    if (args.has("memory-domain")) {
      const auto parsed = coherence::parse_memory_domain(args.get("memory-domain"));
      if (!parsed.has_value()) {
        emit("error code=invalid_argument message=unknown memory domain");
        return 1;
      }
      registration.memory_domain = parsed.value();
    } else {
      registration.memory_domain = coherence::MemoryDomain::Remote;
    }
    if (args.has("evidence")) {
      const auto parsed = coherence::parse_evidence_class(args.get("evidence"));
      registration.evidence_class =
          parsed.has_value() ? parsed.value() : coherence::EvidenceClass::Unsupported;
    } else {
      // The CLI does not hold these bytes; it records a declaration only and
      // must not claim real evidence for memory it cannot read.
      registration.evidence_class = coherence::EvidenceClass::Synthetic;
    }
    registration.offset = args.number("offset");
    registration.length = args.number("length");
    registration.address_hint = args.number("address");
    registration.declared_writable = args.get("writable", "true") == "true";
    registration.content.crc32c = static_cast<std::uint32_t>(args.number("content-crc"));
    registration.content.length = args.number("content-length");
    registration.content.defined = args.has("content-crc");
    coherence::ByteWriter body;
    coherence::encode_region_registration(body, registration);
    return exchange_record<coherence::RegionRecord>(state, coherence::MessageType::RegisterRegion,
                                                    body.span(), coherence::decode_region,
                                                    coherence::render_region);
  }
  if (group == "region" && verb == "retire") {
    coherence::ByteWriter body;
    body.u64(args.number("region"));
    body.u64(args.number("generation"));
    return exchange_text(state, coherence::MessageType::RetireRegion, body.span());
  }
  if (group == "acquire" && verb == "read") {
    coherence::ReadRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.region = coherence::RegionId::from_value(args.number("region"));
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.snapshot_version = coherence::VersionId::from_value(args.number("snapshot-version"));
    request.require_current = true;
    coherence::ByteWriter body;
    coherence::encode_read_request(body, request);
    return exchange_record<coherence::ReadDecision>(state, coherence::MessageType::AcquireRead,
                                                    body.span(), coherence::decode_read_decision,
                                                    coherence::render_read_decision);
  }
  if (group == "acquire" && verb == "write") {
    coherence::WriteRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.region = coherence::RegionId::from_value(args.number("region"));
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    coherence::ByteWriter body;
    coherence::encode_write_request(body, request);
    return exchange_record<coherence::WriteGrant>(state, coherence::MessageType::AcquireWrite,
                                                  body.span(), coherence::decode_write_grant,
                                                  coherence::render_write_grant);
  }
  if (group == "publish") {
    coherence::PublishRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.ownership_generation =
        coherence::OwnershipGeneration::from_value(args.number("ownership"));
    request.region = coherence::RegionId::from_value(args.number("region"));
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.expected_base_version = coherence::VersionId::from_value(args.number("base-version"));
    request.allow_without_dirty = args.has("allow-initial");
    request.content.defined = args.has("content-crc");
    request.content.crc32c = static_cast<std::uint32_t>(args.number("content-crc"));
    request.content.length = args.number("content-length");
    coherence::ByteWriter body;
    coherence::encode_publish_request(body, request);
    return exchange_record<coherence::PublicationReceipt>(
        state, coherence::MessageType::Publish, body.span(), coherence::decode_publication,
        coherence::render_publication);
  }
  if (group == "invalidate") {
    coherence::InvalidationRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.ownership_generation =
        coherence::OwnershipGeneration::from_value(args.number("ownership"));
    request.target_region = coherence::RegionId::from_value(args.number("region"));
    request.target_region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.target_replica_generation =
        coherence::ReplicaGeneration::from_value(args.number("replica-generation", 1));
    request.published_version = coherence::VersionId::from_value(args.number("version"));
    request.acknowledgement_required = args.get("ack", "true") == "true";
    coherence::ByteWriter body;
    coherence::encode_invalidation_request(body, request);
    return exchange_record<coherence::InvalidationOutcome>(
        state, coherence::MessageType::Invalidate, body.span(),
        coherence::decode_invalidation_outcome, coherence::render_invalidation_outcome);
  }
  if (group == "sync-begin") {
    coherence::SyncRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.source_region = coherence::RegionId::from_value(args.number("source"));
    request.source_region_generation =
        coherence::RegionGeneration::from_value(args.number("source-generation"));
    request.destination_region = coherence::RegionId::from_value(args.number("destination"));
    request.destination_region_generation =
        coherence::RegionGeneration::from_value(args.number("destination-generation"));
    request.ownership_generation =
        coherence::OwnershipGeneration::from_value(args.number("ownership"));
    request.transport = args.get("transport", "delegated");
    coherence::ByteWriter body;
    coherence::encode_sync_request(body, request);
    return exchange_record<coherence::SyncOutcome>(state, coherence::MessageType::SyncBegin,
                                                   body.span(), coherence::decode_sync_outcome,
                                                   coherence::render_sync_outcome);
  }
  if (group == "sync-complete") {
    coherence::SyncCompleteRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.operation = coherence::SyncOperationId::from_value(args.number("operation"));
    request.destination_region = coherence::RegionId::from_value(args.number("destination"));
    request.destination_region_generation =
        coherence::RegionGeneration::from_value(args.number("destination-generation"));
    request.destination_new_version = coherence::VersionId::from_value(args.number("version"));
    request.content_moved = args.get("content-moved", "true") == "true";
    request.observed_content.defined = args.has("content-crc");
    request.observed_content.crc32c = static_cast<std::uint32_t>(args.number("content-crc"));
    request.observed_content.length = args.number("content-length");
    coherence::ByteWriter body;
    coherence::encode_sync_complete(body, request);
    return exchange_record<coherence::SyncOutcome>(state, coherence::MessageType::SyncComplete,
                                                   body.span(), coherence::decode_sync_outcome,
                                                   coherence::render_sync_outcome);
  }
  if (group == "revalidate") {
    coherence::RevalidateRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.region = coherence::RegionId::from_value(args.number("region"));
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.observed_version = coherence::VersionId::from_value(args.number("version"));
    request.content.defined = args.has("content-crc");
    request.content.crc32c = static_cast<std::uint32_t>(args.number("content-crc"));
    request.content.length = args.number("content-length");
    request.byte_compared = args.get("byte-compared", "true") == "true";
    coherence::ByteWriter body;
    coherence::encode_revalidate_request(body, request);
    return exchange_record<coherence::RegionRecord>(state, coherence::MessageType::Revalidate,
                                                    body.span(), coherence::decode_region,
                                                    coherence::render_region);
  }
  if (group == "release") {
    coherence::ReleaseRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.lease = coherence::LeaseId::from_value(args.number("lease"));
    request.ownership_generation =
        coherence::OwnershipGeneration::from_value(args.number("ownership"));
    request.release_write_authority = args.has("write");
    coherence::ByteWriter body;
    coherence::encode_release_request(body, request);
    return exchange_record<coherence::ReleaseOutcome>(state, coherence::MessageType::Release,
                                                      body.span(),
                                                      coherence::decode_release_outcome,
                                                      render_release_record);
  }
  if (group == "mark-dirty") {
    coherence::DirtyRequest request;
    request.context.epoch = state.client.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    request.context.object_generation =
        coherence::ObjectGeneration::from_value(args.number("object-generation", 1));
    request.context.request = coherence::generate_request_id();
    request.region = coherence::RegionId::from_value(args.number("region"));
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.ownership_generation =
        coherence::OwnershipGeneration::from_value(args.number("ownership"));
    request.base_version = coherence::VersionId::from_value(args.number("base-version"));
    coherence::ByteWriter body;
    coherence::encode_dirty_request(body, request);
    return exchange_text(state, coherence::MessageType::MarkDirty, body.span());
  }
  if (group == "participant" && verb == "fence") {
    coherence::ByteWriter body;
    body.strong_id(coherence::ParticipantId::from_value(args.number("id")));
    body.text(args.get("reason", "fenced by operator"));
    return exchange_record<coherence::ParticipantRecord>(
        state, coherence::MessageType::FenceParticipant, body.span(),
        coherence::decode_participant, coherence::render_participant);
  }

  std::fprintf(stderr,
               "coherence_cli: command '%s %s' is not available against a remote coordinator\n",
               group.c_str(), verb.c_str());
  return 2;
}
