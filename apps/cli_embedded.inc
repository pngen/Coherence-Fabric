// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
// Embedded command implementations for coherence_cli. Included by the app.

int run_embedded(Embedded& state, const Args& args) {
  coherence::CoherenceEngine& engine = *state.engine;
  const std::string group = args.positional[0];
  const std::string verb = args.positional.size() > 1 ? args.positional[1] : std::string();

  if (group == "status") {
    emit(std::string("build=") + coherence::build_identification());
    emit("epoch=" + engine.epoch().to_string());
    emit("sequence=" + engine.sequence().to_string());
    emit(state.durable ? "durable=file" : "durable=ephemeral");
    auto audit = engine.audit();
    emit(std::string("invariant_audit=") + (audit.clean ? "clean" : "violations"));
    return 0;
  }
  if (group == "domain" && verb == "create") {
    auto created = engine.create_domain(args.get("name", "default"));
    if (!created.has_value()) {
      emit_failure(created.status());
      return 1;
    }
    state.domain = created.value().id;
    emit("domain id=" + created.value().id.to_string() + " name=" + created.value().name);
    return 0;
  }
  if (group == "domain" && verb == "show") {
    auto snapshot = engine.snapshot(coherence::SnapshotOptions{});
    if (!snapshot.has_value()) {
      emit_failure(snapshot.status());
      return 1;
    }
    for (const coherence::DomainRecord& record : snapshot.value().domains) {
      emit("domain id=" + record.id.to_string() + " name=" + record.name +
           " lifecycle=" + std::string(coherence::to_token(record.lifecycle)) +
           " created_epoch=" + record.created_epoch.to_string() +
           " current_epoch=" + record.current_epoch.to_string());
    }
    return 0;
  }
  if (group == "object" && verb == "create") {
    coherence::CoherenceDomainId domain = coherence::CoherenceDomainId::from_value(args.number("domain"));
    if (!domain.defined()) domain = state.domain;
    if (!domain.defined()) {
      auto created = engine.create_domain("default");
      if (!created.has_value()) {
        emit_failure(created.status());
        return 1;
      }
      domain = created.value().id;
      state.domain = domain;
    }
    auto object = engine.register_object(domain, args.get("name", "object"), args.number("length"),
                                         build_policy(args));
    if (!object.has_value()) {
      emit_failure(object.status());
      return 1;
    }
    emit("object id=" + object.value().id.to_string() + " generation=" +
         object.value().generation.to_string() + " name=" + object.value().name +
         " length=" + std::to_string(object.value().length) +
         " policy=" + object.value().policy.to_string() +
         " policy_generation=" + object.value().policy_generation.to_string());
    return 0;
  }
  if (group == "object" && (verb == "show" || verb == "list")) {
    coherence::SnapshotOptions options;
    options.object_filter = coherence::ObjectId::from_value(args.number("object"));
    auto snapshot = engine.snapshot(options);
    if (!snapshot.has_value()) {
      emit_failure(snapshot.status());
      return 1;
    }
    for (const coherence::ObjectRecord& record : snapshot.value().objects) {
      emit(coherence::render_object(record));
    }
    return 0;
  }
  if (group == "object" && verb == "resolve-recovery") {
    const coherence::ObjectId object = coherence::ObjectId::from_value(args.number("object"));
    auto record = engine.get_object(object);
    if (!record.has_value()) {
      emit_failure(record.status());
      return 1;
    }
    auto resolved = engine.resolve_recovery(
        object,
        coherence::ObjectGeneration::from_value(args.number("generation",
                                                             record.value().generation.value())),
        engine.epoch(), args.get("note", ""));
    if (!resolved.has_value()) {
      emit_failure(resolved.status());
      return 1;
    }
    emit(coherence::render_object(resolved.value()));
    return 0;
  }
  if (group == "object" && verb == "retire") {
    Status retired = engine.retire_object(
        coherence::ObjectId::from_value(args.number("object")),
        coherence::ObjectGeneration::from_value(args.number("generation")), engine.epoch());
    if (!retired.ok()) {
      emit_failure(retired);
      return 1;
    }
    emit("object retired");
    return 0;
  }
  if (group == "policy" && verb == "show") {
    auto policy = engine.get_policy(coherence::ObjectId::from_value(args.number("object")));
    if (!policy.has_value()) {
      emit_failure(policy.status());
      return 1;
    }
    emit(coherence::render_policy(policy.value()));
    return 0;
  }
  if (group == "region" && verb == "register") {
    const coherence::ObjectId object = coherence::ObjectId::from_value(args.number("object"));
    auto object_record = engine.get_object(object);
    if (!object_record.has_value()) {
      emit_failure(object_record.status());
      return 1;
    }
    const std::uint64_t length = args.number("length");
    const std::uint64_t offset = args.number("offset");
    coherence::MemoryDomain domain = coherence::MemoryDomain::HostPageable;
    if (args.has("memory-domain")) {
      const auto parsed = coherence::parse_memory_domain(args.get("memory-domain"));
      if (!parsed.has_value()) {
        emit("error code=invalid_argument message=unknown memory domain");
        return 1;
      }
      domain = parsed.value();
    }
    const std::string name = args.get("name", "replica");
    auto created = [&]() -> Result<coherence::RegionId> {
      if (domain == coherence::MemoryDomain::CxlClass) {
        return state.store.add_synthetic_cxl_region(name, length, args.number("seed", 1));
      }
      return state.store.add_host_region(name, length, domain, args.number("seed", 1));
    }();
    if (!created.has_value()) {
      emit_failure(created.status());
      return 1;
    }
    coherence::RegionRegistration registration;
    registration.context = authority_for(state, object);
    registration.domain = object_record.value().domain;
    registration.requested_id = created.value();
    registration.name = name;
    const coherence::adapters::StoredRegion& stored = state.store.at(created.value());
    registration.memory_domain = stored.memory_domain;
    registration.evidence_class = stored.evidence_class;
    registration.offset = offset;
    registration.length = length;
    registration.address_hint = args.number("address", stored.address_hint);
    registration.content = coherence::fingerprint_bytes(stored.bytes());
    auto region = engine.register_region(registration);
    if (!region.has_value()) {
      emit_failure(region.status());
      return 1;
    }
    state.region_names[name] = region.value().id;
    emit(coherence::render_region(region.value()));
    return 0;
  }
  if (group == "region" && (verb == "list" || verb == "show")) {
    coherence::SnapshotOptions options;
    options.object_filter = coherence::ObjectId::from_value(args.number("object"));
    auto snapshot = engine.snapshot(options);
    if (!snapshot.has_value()) {
      emit_failure(snapshot.status());
      return 1;
    }
    for (const coherence::RegionRecord& record : snapshot.value().regions) {
      emit(coherence::render_region(record));
    }
    return 0;
  }
  if (group == "region" && verb == "retire") {
    Status retired = engine.retire_region(
        coherence::RegionId::from_value(args.number("region")),
        coherence::RegionGeneration::from_value(args.number("generation")), engine.epoch());
    if (!retired.ok()) {
      emit_failure(retired);
      return 1;
    }
    emit("region retired");
    return 0;
  }
  if (group == "acquire" && (verb == "read" || verb == "write")) {
    const coherence::ObjectId object = coherence::ObjectId::from_value(args.number("object"));
    if (verb == "read") {
      coherence::ReadRequest request;
      request.context = authority_for(state, object);
      request.region = coherence::RegionId::from_value(args.number("region"));
      request.region_generation =
          coherence::RegionGeneration::from_value(args.number("region-generation"));
      request.snapshot_version = coherence::VersionId::from_value(args.number("snapshot-version"));
      request.require_current = true;
      auto decision = engine.acquire_read(request);
      if (!decision.has_value()) {
        emit_failure(decision.status());
        return 1;
      }
      std::string text = coherence::render_read_decision(decision.value());
      std::size_t start = 0;
      while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        emit(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
      }
      return decision.value().outcome == coherence::ReadOutcome::ReadCurrent ||
                     decision.value().outcome == coherence::ReadOutcome::ReadStaleAllowed
                 ? 0
                 : 3;
    }
    coherence::WriteRequest request;
    request.context = authority_for(state, object);
    request.region = coherence::RegionId::from_value(args.number("region"));
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    auto grant = engine.acquire_write(request);
    if (!grant.has_value()) {
      emit_failure(grant.status());
      return 1;
    }
    emit(coherence::render_write_grant(grant.value()));
    return grant.value().granted ? 0 : 3;
  }
  if (group == "mark-dirty" || (group == "revalidate")) {
    const coherence::ObjectId object = coherence::ObjectId::from_value(args.number("object"));
    const coherence::RegionId region = coherence::RegionId::from_value(args.number("region"));
    if (group == "mark-dirty") {
      coherence::DirtyRequest request;
      request.context = authority_for(state, object);
      request.region = region;
      request.region_generation =
          coherence::RegionGeneration::from_value(args.number("region-generation"));
      request.ownership_generation =
          coherence::OwnershipGeneration::from_value(args.number("ownership"));
      request.base_version = coherence::VersionId::from_value(args.number("base-version"));
      auto result = engine.mark_dirty(request);
      if (!result.has_value()) {
        emit_failure(result.status());
        return 1;
      }
      emit(std::string("dirty_condition=") + std::string(coherence::to_token(result.value())));
      return 0;
    }
    coherence::RevalidateRequest request;
    request.context = authority_for(state, object);
    request.region = region;
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.observed_version = coherence::VersionId::from_value(args.number("version"));
    request.content.length = args.number("content-length");
    request.content.defined = args.has("content-crc");
    request.content.crc32c = static_cast<std::uint32_t>(args.number("content-crc"));
    request.byte_compared = args.get("byte-compared", "true") == "true";
    const coherence::adapters::StoredRegion* stored = state.store.find(region);
    if (stored != nullptr) request.content = coherence::fingerprint_bytes(stored->bytes());
    auto updated = engine.revalidate_region(request);
    if (!updated.has_value()) {
      emit_failure(updated.status());
      return 1;
    }
    emit(coherence::render_region(updated.value()));
    return 0;
  }
  if (group == "publish") {
    const coherence::ObjectId object = coherence::ObjectId::from_value(args.number("object"));
    coherence::PublishRequest request;
    request.context = authority_for(state, object);
    request.ownership_generation =
        coherence::OwnershipGeneration::from_value(args.number("ownership"));
    request.region = coherence::RegionId::from_value(args.number("region"));
    request.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.expected_base_version = coherence::VersionId::from_value(args.number("base-version"));
    request.allow_without_dirty = args.has("allow-initial");
    const coherence::adapters::StoredRegion* stored = state.store.find(request.region);
    if (stored != nullptr) request.content = coherence::fingerprint_bytes(stored->bytes());
    auto receipt = engine.publish(request);
    if (!receipt.has_value()) {
      emit_failure(receipt.status());
      return 1;
    }
    std::string text = coherence::render_publication(receipt.value());
    std::size_t start = 0;
    while (start <= text.size()) {
      const std::size_t end = text.find('\n', start);
      emit(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
      if (end == std::string::npos) break;
      start = end + 1;
    }
    return receipt.value().state == coherence::PublicationState::Committed ? 0 : 3;
  }
  if (group == "invalidate") {
    coherence::InvalidationRequest request;
    request.context.epoch = engine.epoch();
    request.context.object = coherence::ObjectId::from_value(args.number("object"));
    auto object_record = engine.get_object(request.context.object);
    if (object_record.has_value()) {
      request.context.object_generation = object_record.value().generation;
      request.context.policy_generation = object_record.value().policy_generation;
      request.ownership_generation = object_record.value().ownership_generation;
      request.published_version = object_record.value().authoritative_version;
    }
    request.target_region = coherence::RegionId::from_value(args.number("region"));
    request.target_region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    request.target_replica_generation =
        coherence::ReplicaGeneration::from_value(args.number("replica-generation", 1));
    request.acknowledgement_required = args.get("ack", "true") == "true";
    auto outcome = engine.invalidate(request);
    if (!outcome.has_value()) {
      emit_failure(outcome.status());
      return 1;
    }
    emit(coherence::render_invalidation_outcome(outcome.value()));
    return 0;
  }
  if (group == "ack-invalidation") {
    coherence::InvalidationAck acknowledgement;
    acknowledgement.context = authority_for(
        state, coherence::ObjectId::from_value(args.number("object")));
    acknowledgement.invalidation =
        coherence::InvalidationId::from_value(args.number("invalidation"));
    acknowledgement.region = coherence::RegionId::from_value(args.number("region"));
    acknowledgement.region_generation =
        coherence::RegionGeneration::from_value(args.number("region-generation"));
    acknowledgement.replica_generation =
        coherence::ReplicaGeneration::from_value(args.number("replica-generation"));
    acknowledgement.superseded_version =
        coherence::VersionId::from_value(args.number("superseded-version"));
    auto outcome = engine.acknowledge_invalidation(acknowledgement);
    if (!outcome.has_value()) {
      emit_failure(outcome.status());
      return 1;
    }
    emit(coherence::render_invalidation_outcome(outcome.value()));
    return 0;
  }
  if (group == "sync-begin" || group == "sync-complete") {
    const coherence::ObjectId object = coherence::ObjectId::from_value(args.number("object"));
    if (group == "sync-begin") {
      coherence::SyncRequest request;
      request.context = authority_for(state, object);
      request.source_region = coherence::RegionId::from_value(args.number("source"));
      request.source_region_generation =
          coherence::RegionGeneration::from_value(args.number("source-generation"));
      request.destination_region = coherence::RegionId::from_value(args.number("destination"));
      request.destination_region_generation =
          coherence::RegionGeneration::from_value(args.number("destination-generation"));
      request.ownership_generation =
          coherence::OwnershipGeneration::from_value(args.number("ownership"));
      request.transport = args.get("transport", "delegated");
      auto outcome = engine.begin_sync(request);
      if (!outcome.has_value()) {
        emit_failure(outcome.status());
        return 1;
      }
      emit(coherence::render_sync_outcome(outcome.value()));
      return 0;
    }
    coherence::SyncCompleteRequest request;
    request.context = authority_for(state, object);
    request.operation = coherence::SyncOperationId::from_value(args.number("operation"));
    request.destination_region = coherence::RegionId::from_value(args.number("destination"));
    request.destination_region_generation =
        coherence::RegionGeneration::from_value(args.number("destination-generation"));
    request.destination_new_version = coherence::VersionId::from_value(args.number("version"));
    request.content_moved = args.get("content-moved", "true") == "true";
    const coherence::adapters::StoredRegion* stored = state.store.find(request.destination_region);
    if (stored != nullptr) request.observed_content = coherence::fingerprint_bytes(stored->bytes());
    auto outcome = engine.complete_sync(request);
    if (!outcome.has_value()) {
      emit_failure(outcome.status());
      return 1;
    }
    emit(coherence::render_sync_outcome(outcome.value()));
    return outcome.value().state == coherence::SyncState::Completed ? 0 : 3;
  }
  if (group == "release") {
    const coherence::ObjectId object = coherence::ObjectId::from_value(args.number("object"));
    coherence::ReleaseRequest request;
    request.context = authority_for(state, object);
    request.lease = coherence::LeaseId::from_value(args.number("lease"));
    request.ownership_generation =
        coherence::OwnershipGeneration::from_value(args.number("ownership"));
    request.release_write_authority = args.has("write");
    auto outcome = engine.release(request);
    if (!outcome.has_value()) {
      emit_failure(outcome.status());
      return 1;
    }
    emit("release reason=" + std::string(coherence::status_code_name(outcome.value().reason)) +
         " rationale=" + outcome.value().rationale);
    return 0;
  }
  if (group == "participant" && verb == "register") {
    auto participant = engine.register_participant(
        args.get("name", "participant"), coherence::generate_participant_boot_id(),
        engine.epoch(), args.get("node", ""));
    if (!participant.has_value()) {
      emit_failure(participant.status());
      return 1;
    }
    emit(coherence::render_participant(participant.value()));
    return 0;
  }
  if (group == "participant" && (verb == "list" || verb == "show")) {
    auto snapshot = engine.snapshot(coherence::SnapshotOptions{});
    if (!snapshot.has_value()) {
      emit_failure(snapshot.status());
      return 1;
    }
    for (const coherence::ParticipantRecord& record : snapshot.value().participants) {
      if (verb == "show" && record.id.value() != args.number("id")) continue;
      emit(coherence::render_participant(record));
    }
    return 0;
  }
  if (group == "participant" && verb == "fence") {
    auto fenced = engine.fence_participant(
        coherence::ParticipantId::from_value(args.number("id")), engine.epoch(),
        args.get("reason", "fenced by operator"));
    if (!fenced.has_value()) {
      emit_failure(fenced.status());
      return 1;
    }
    emit(coherence::render_participant(fenced.value()));
    return 0;
  }
  if (group == "snapshot") {
    coherence::SnapshotOptions options;
    options.object_filter = coherence::ObjectId::from_value(args.number("object"));
    auto snapshot = engine.snapshot(options);
    if (!snapshot.has_value()) {
      emit_failure(snapshot.status());
      return 1;
    }
    emit(snapshot.value().render());
    return 0;
  }
  if (group == "decisions") {
    emit(engine.render_decision_log());
    return 0;
  }
  if (group == "audit" || group == "verify") {
    const coherence::AuditReport report = engine.audit();
    emit(report.render());
    return report.clean ? 0 : 4;
  }
  if (group == "recovery") {
    auto missed = engine.snapshot(coherence::SnapshotOptions{});
    (void)missed;
    emit("recovery embedded=1");
    return 0;
  }
  if (group == "demo") {
    return run_demo(state, args);
  }

  std::fprintf(stderr, "coherence_cli: unrecognised command '%s'\n", group.c_str());
  return 2;
}
