// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
// The coherence_cli demonstration: a complete lifecycle over real bytes.
//
// Every step is asserted. The demo exits non-zero if any expected outcome does
// not occur, so it is a proof rather than a transcript.

namespace {

struct DemoReport {
  int steps = 0;
  int failures = 0;

  void check(const std::string& name, bool ok, const std::string& detail) {
    ++steps;
    if (!ok) ++failures;
    std::printf("demo step=%s ok=%s detail=%s\n", name.c_str(), ok ? "true" : "false",
                detail.c_str());
    std::fflush(stdout);
  }
};

coherence::RegionId demo_register_region(Embedded& state, coherence::ObjectId object,
                                         const std::string& name, std::uint64_t length,
                                         coherence::MemoryDomain domain,
                                         coherence::EvidenceClass evidence_class,
                                         coherence::VersionId initial_version) {
  auto created = domain == coherence::MemoryDomain::CxlClass
                     ? state.store.add_synthetic_cxl_region(name, length, 7)
                     : state.store.add_host_region(name, length, domain, 7);
  if (!created.has_value()) return coherence::RegionId::nil();
  state.store.at(created.value()).evidence_class = evidence_class;
  coherence::RegionRegistration registration;
  registration.context = authority_for(state, object);
  auto object_record = state.engine->get_object(object);
  registration.domain = object_record.value().domain;
  registration.requested_id = created.value();
  registration.name = name;
  registration.memory_domain = domain;
  registration.evidence_class = evidence_class;
  registration.offset = 0;
  registration.length = length;
  registration.address_hint = state.store.at(created.value()).address_hint;
  registration.initial_version = initial_version;
  registration.content = coherence::fingerprint_bytes(state.store.at(created.value()).bytes());
  auto region = state.engine->register_region(registration);
  if (!region.has_value()) return coherence::RegionId::nil();
  state.region_names[name] = region.value().id;
  return region.value().id;
}

} // namespace

int run_demo(Embedded& state, const Args& args) {
  (void)args;
  coherence::CoherenceEngine& engine = *state.engine;
  DemoReport report;

  auto domain = engine.create_domain("demo");
  report.check("domain_create", domain.has_value(),
               domain.has_value() ? domain.value().id.to_string() : domain.status().to_string());
  if (!domain.has_value()) return 1;
  state.domain = domain.value().id;

  auto object = engine.register_object(domain.value().id, "demo-object", 4096,
                                       coherence::strict_policy("demo-strict"));
  report.check("object_create", object.has_value(),
               object.has_value() ? object.value().id.to_string() : object.status().to_string());
  if (!object.has_value()) return 1;
  const coherence::ObjectId object_id = object.value().id;

  const coherence::RegionId region_a =
      demo_register_region(state, object_id, "replica-a", 4096,
                           coherence::MemoryDomain::HostPageable, coherence::EvidenceClass::Real,
                           coherence::VersionId::nil());
  const coherence::RegionId region_b =
      demo_register_region(state, object_id, "replica-b", 4096,
                           coherence::MemoryDomain::HostPageable, coherence::EvidenceClass::Real,
                           coherence::VersionId::nil());
  const coherence::RegionId region_synthetic =
      demo_register_region(state, object_id, "replica-synthetic-cxl", 4096,
                           coherence::MemoryDomain::CxlClass, coherence::EvidenceClass::Synthetic,
                           coherence::VersionId::nil());
  report.check("region_registration",
               region_a.defined() && region_b.defined() && region_synthetic.defined(),
               "a=" + region_a.to_string() + " b=" + region_b.to_string() +
                   " synthetic=" + region_synthetic.to_string());

  // A newly registered replica is never current merely because it exists.
  const auto before = engine.get_region(region_a);
  report.check("new_replica_is_not_current",
               before.has_value() && before.value().state != coherence::CoherenceState::Current,
               before.has_value() ? std::string(coherence::to_token(before.value().state))
                                  : before.status().to_string());

  auto grant_write = [&](coherence::RegionId region) {
    coherence::WriteRequest request;
    request.context = authority_for(state, object_id);
    request.region = region;
    request.region_generation = engine.get_region(region).value().generation;
    return engine.acquire_write(request);
  };
  auto release_write = [&](coherence::OwnershipGeneration ownership) {
    coherence::ReleaseRequest request;
    request.context = authority_for(state, object_id);
    request.ownership_generation = ownership;
    request.release_write_authority = true;
    return engine.release(request);
  };

  // Establish the initial authoritative version from a direct observation of
  // real bytes held by replica A.
  {
    coherence::RevalidateRequest request;
    request.context = authority_for(state, object_id);
    request.region = region_a;
    request.region_generation = before.value().generation;
    request.observed_version = coherence::VersionId::from_value(1);
    request.content = coherence::fingerprint_bytes(state.store.at(region_a).bytes());
    const auto updated = engine.revalidate_region(request);
    report.check("revalidate_initial",
                 updated.has_value() && updated.value().state == coherence::CoherenceState::Current,
                 updated.has_value() ? std::string(coherence::to_token(updated.value().state))
                                     : updated.status().to_string());
  }

  const auto first_grant = grant_write(region_a);
  report.check("initial_write_authority",
               first_grant.has_value() && first_grant.value().granted &&
                   first_grant.value().may_mutate_now,
               first_grant.has_value()
                   ? std::string(coherence::status_code_name(first_grant.value().reason))
                   : first_grant.status().to_string());

  coherence::VersionId version;
  {
    coherence::PublishRequest request;
    request.context = authority_for(state, object_id);
    request.ownership_generation = first_grant.value().context.ownership_generation;
    request.region = region_a;
    request.region_generation = engine.get_region(region_a).value().generation;
    request.expected_base_version = coherence::VersionId::nil();
    request.allow_without_dirty = true;
    request.content = coherence::fingerprint_bytes(state.store.at(region_a).bytes());
    const auto receipt = engine.publish(request);
    report.check("initial_publication",
                 receipt.has_value() &&
                     receipt.value().state == coherence::PublicationState::Committed,
                 receipt.has_value() ? coherence::status_code_name(receipt.value().reason).data()
                                     : receipt.status().to_string());
    if (receipt.has_value()) version = receipt.value().version;
  }

  // Replica B holds nothing that matches the authoritative version, so a strict
  // read must not report it as current.
  {
    coherence::ReadRequest request;
    request.context = authority_for(state, object_id);
    request.region = region_b;
    request.require_current = true;
    const auto decision = engine.acquire_read(request);
    report.check("never_synchronized_replica_is_not_current",
                 decision.has_value() &&
                     decision.value().outcome == coherence::ReadOutcome::ReadAfterSync,
                 decision.has_value() ? std::string(coherence::to_token(decision.value().outcome))
                                      : decision.status().to_string());
  }

  // Real byte movement from A into B, verified by content comparison.
  coherence::SyncOperationId operation;
  {
    coherence::SyncRequest request;
    request.context = authority_for(state, object_id);
    request.source_region = region_a;
    request.source_region_generation = engine.get_region(region_a).value().generation;
    request.destination_region = region_b;
    request.destination_region_generation = engine.get_region(region_b).value().generation;
    request.ownership_generation = engine.get_object(object_id).value().ownership_generation;
    request.transport = "in_process_byte_copy";
    const auto outcome = engine.begin_sync(request);
    report.check("synchronization_plan_issued",
                 outcome.has_value() && outcome.value().state == coherence::SyncState::Requested,
                 outcome.has_value() ? coherence::status_code_name(outcome.value().reason).data()
                                     : outcome.status().to_string());
    if (outcome.has_value()) operation = outcome.value().operation;
  }
  {
    const Status copied = state.store.copy_and_verify(region_a, region_b);
    report.check("real_byte_copy_verified", copied.ok(), copied.to_string());
  }
  {
    coherence::SyncCompleteRequest request;
    request.context = authority_for(state, object_id);
    request.operation = operation;
    request.destination_region = region_b;
    request.destination_region_generation = engine.get_region(region_b).value().generation;
    request.destination_new_version = version;
    request.observed_content = coherence::fingerprint_bytes(state.store.at(region_b).bytes());
    const auto outcome = engine.complete_sync(request);
    report.check("sync_completion_certifies_currentness",
                 outcome.has_value() && outcome.value().state == coherence::SyncState::Completed,
                 outcome.has_value() ? coherence::status_code_name(outcome.value().reason).data()
                                     : outcome.status().to_string());
  }
  {
    coherence::ReadRequest request;
    request.context = authority_for(state, object_id);
    request.region = region_b;
    const auto decision = engine.acquire_read(request);
    report.check("read_current_after_synchronization",
                 decision.has_value() &&
                     decision.value().outcome == coherence::ReadOutcome::ReadCurrent &&
                     decision.value().authoritative_version == version,
                 decision.has_value() ? std::string(coherence::to_token(decision.value().outcome))
                                      : decision.status().to_string());
  }

  // Hand write authority from A to B. A must be invalidated and must
  // acknowledge before B may mutate.
  {
    const auto released = release_write(first_grant.value().context.ownership_generation);
    report.check("first_writer_released_authority", released.has_value(),
                 released.has_value() ? released.value().rationale
                                      : released.status().to_string());
  }
  coherence::InvalidationId invalidation;
  auto second_grant = grant_write(region_b);
  {
    const bool reserved = second_grant.has_value() && second_grant.value().granted &&
                          !second_grant.value().may_mutate_now &&
                          second_grant.value().required_invalidations.size() == 1;
    report.check("write_transfer_requires_acknowledgement", reserved,
                 second_grant.has_value()
                     ? std::string(coherence::status_code_name(second_grant.value().reason))
                     : second_grant.status().to_string());
    if (reserved) invalidation = second_grant.value().required_invalidations.front().id;
  }
  {
    coherence::PublishRequest request;
    request.context = authority_for(state, object_id);
    request.ownership_generation = second_grant.value().context.ownership_generation;
    request.region = region_b;
    request.region_generation = engine.get_region(region_b).value().generation;
    request.expected_base_version = version;
    request.allow_without_dirty = true;
    request.content = coherence::fingerprint_bytes(state.store.at(region_b).bytes());
    const auto receipt = engine.publish(request);
    report.check("publication_blocked_while_invalidation_outstanding",
                 receipt.has_value() &&
                     receipt.value().state == coherence::PublicationState::None &&
                     receipt.value().reason == coherence::StatusCode::InvalidationOutstanding,
                 receipt.has_value() ? coherence::status_code_name(receipt.value().reason).data()
                                     : receipt.status().to_string());
  }
  {
    auto snapshot = engine.snapshot(coherence::SnapshotOptions{});
    coherence::InvalidationRecord target{};
    bool found = false;
    for (const coherence::InvalidationRecord& candidate : snapshot.value().invalidations) {
      if (candidate.id == invalidation) {
        target = candidate;
        found = true;
      }
    }
    coherence::InvalidationAck acknowledgement;
    acknowledgement.context = authority_for(state, object_id);
    acknowledgement.invalidation = invalidation;
    acknowledgement.region = target.region;
    acknowledgement.region_generation = target.region_generation;
    acknowledgement.replica_generation = target.replica_generation;
    acknowledgement.superseded_version = target.superseded_version;
    const auto outcome = engine.acknowledge_invalidation(acknowledgement);
    report.check("invalidation_acknowledged",
                 found && outcome.has_value() &&
                     outcome.value().state == coherence::InvalidationState::Acknowledged,
                 found ? (outcome.has_value()
                              ? std::string(coherence::to_token(outcome.value().state))
                              : outcome.status().to_string())
                       : "invalidation record not found");
    const auto invalidated = engine.get_region(region_a);
    report.check("invalidated_replica_is_not_current",
                 invalidated.has_value() &&
                     invalidated.value().state != coherence::CoherenceState::Current,
                 invalidated.has_value()
                     ? std::string(coherence::to_token(invalidated.value().state))
                     : invalidated.status().to_string());
  }
  second_grant = grant_write(region_b);
  report.check("write_authority_effective_after_acknowledgement",
               second_grant.has_value() && second_grant.value().granted &&
                   second_grant.value().may_mutate_now,
               second_grant.has_value()
                   ? std::string(coherence::status_code_name(second_grant.value().reason))
                   : second_grant.status().to_string());

  // Mutate real bytes, mark dirty, publish a new authoritative version.
  coherence::VersionId version2;
  {
    const Status poked = state.store.poke(region_b, 0, 0xAB, 64);
    report.check("real_byte_mutation", poked.ok(), poked.to_string());
    coherence::DirtyRequest request;
    request.context = authority_for(state, object_id);
    request.region = region_b;
    request.region_generation = engine.get_region(region_b).value().generation;
    request.ownership_generation = second_grant.value().context.ownership_generation;
    request.base_version = version;
    const auto dirty = engine.mark_dirty(request);
    report.check("mark_dirty", dirty.has_value(),
                 dirty.has_value() ? std::string(coherence::to_token(dirty.value()))
                                   : dirty.status().to_string());
  }
  {
    coherence::PublishRequest request;
    request.context = authority_for(state, object_id);
    request.ownership_generation = second_grant.value().context.ownership_generation;
    request.region = region_b;
    request.region_generation = engine.get_region(region_b).value().generation;
    request.expected_base_version = version;
    request.content = coherence::fingerprint_bytes(state.store.at(region_b).bytes());
    const auto receipt = engine.publish(request);
    report.check("second_publication",
                 receipt.has_value() &&
                     receipt.value().state == coherence::PublicationState::Committed &&
                     receipt.value().version == version.next(),
                 receipt.has_value() ? receipt.value().version.to_string()
                                     : receipt.status().to_string());
    if (receipt.has_value()) version2 = receipt.value().version;
  }
  {
    coherence::ReadRequest request;
    request.context = authority_for(state, object_id);
    request.region = region_a;
    request.require_current = true;
    const auto decision = engine.acquire_read(request);
    report.check("superseded_replica_is_never_reported_current",
                 decision.has_value() &&
                     decision.value().outcome != coherence::ReadOutcome::ReadCurrent,
                 decision.has_value() ? std::string(coherence::to_token(decision.value().outcome))
                                      : decision.status().to_string());
  }

  // A replica backed by a synthetic CXL-class domain carries synthetic evidence
  // and is reported as such.
  {
    const auto synthetic_record = engine.get_region(region_synthetic);
    coherence::RevalidateRequest request;
    request.context = authority_for(state, object_id);
    request.region = region_synthetic;
    request.region_generation = synthetic_record.value().generation;
    request.observed_version = version2;
    request.content = coherence::fingerprint_bytes(state.store.at(region_synthetic).bytes());
    const auto updated = engine.revalidate_region(request);
    report.check("synthetic_cxl_region_revalidated",
                 updated.has_value(),
                 updated.has_value() ? std::string(coherence::to_token(updated.value().state))
                                     : updated.status().to_string());
    report.check("synthetic_cxl_evidence_class",
                 synthetic_record.value().evidence_class == coherence::EvidenceClass::Synthetic &&
                     synthetic_record.value().memory_domain == coherence::MemoryDomain::CxlClass,
                 std::string(coherence::to_token(synthetic_record.value().evidence_class)));
  }

  // Release write authority; the ownership generation advances so the previous
  // authority can never be reused.
  const coherence::OwnershipGeneration before_release =
      engine.get_object(object_id).value().ownership_generation;
  {
    const auto released = release_write(before_release);
    const coherence::OwnershipGeneration after_release =
        engine.get_object(object_id).value().ownership_generation;
    report.check("release_advances_ownership_generation",
                 released.has_value() &&
                     after_release.value() == before_release.value() + 1,
                 "before=" + before_release.to_string() + " after=" + after_release.to_string());
  }
  {
    auto reacquired = grant_write(region_b);
    // The synthetic replica became current while authority was released, so it
    // must be invalidated and must acknowledge before the writer may mutate.
    if (reacquired.has_value() && !reacquired.value().may_mutate_now) {
      for (const coherence::InvalidationRecord& required :
           reacquired.value().required_invalidations) {
        coherence::InvalidationAck acknowledgement;
        acknowledgement.context = authority_for(state, object_id);
        acknowledgement.invalidation = required.id;
        acknowledgement.region = required.region;
        acknowledgement.region_generation = required.region_generation;
        acknowledgement.replica_generation = required.replica_generation;
        acknowledgement.superseded_version = required.superseded_version;
        (void)engine.acknowledge_invalidation(acknowledgement);
      }
      reacquired = grant_write(region_b);
    }
    report.check("write_authority_reacquired",
                 reacquired.has_value() && reacquired.value().may_mutate_now,
                 reacquired.has_value()
                     ? std::string(coherence::status_code_name(reacquired.value().reason))
                     : reacquired.status().to_string());
    if (reacquired.has_value()) {
      coherence::DirtyRequest stale;
      stale.context = authority_for(state, object_id);
      stale.region = region_b;
      stale.region_generation = engine.get_region(region_b).value().generation;
      stale.ownership_generation = before_release;
      stale.base_version = version2;
      const auto rejected = engine.mark_dirty(stale);
      report.check("stale_ownership_generation_rejected", !rejected.has_value() &&
                                                               rejected.status().code() ==
                                                                   coherence::StatusCode::StaleOwnership,
                   rejected.has_value() ? "accepted" : rejected.status().to_string());
    }
  }

  const coherence::AuditReport audit = engine.audit();
  report.check("invariant_audit_clean", audit.clean, audit.render());

  std::printf("demo steps=%d failures=%d\n", report.steps, report.failures);
  std::fflush(stdout);
  return report.failures == 0 ? 0 : 5;
}
