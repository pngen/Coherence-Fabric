// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// The invariant auditor must be machine-checkable: it has to report zero
// violations on a healthy runtime, and it has to detect each invariant when the
// durable state is deliberately inconsistent.
#include <memory>
#include <string>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "test_framework.hpp"

namespace {

coherence::DurableImage base_image() {
  coherence::DurableImage image;
  image.sequence = 4;
  image.epoch = coherence::CoordinatorEpoch::from_value(2);
  coherence::CoherencePolicy policy = coherence::strict_policy("audit");
  policy.id = coherence::PolicyId::from_value(1);
  policy.generation = coherence::PolicyGeneration::from_value(1);
  image.policies.emplace(policy.id, policy);

  coherence::CoherenceDomainId domain_id = coherence::CoherenceDomainId::from_value(1);
  coherence::DomainRecord domain;
  domain.id = domain_id;
  domain.name = "audit";
  domain.generation = 1;
  domain.lifecycle = coherence::DomainLifecycle::Active;
  domain.created_epoch = image.epoch;
  domain.current_epoch = image.epoch;
  image.domains.emplace(domain.id, domain);

  coherence::ParticipantRecord participant;
  participant.id = coherence::ParticipantId::from_value(1);
  participant.name = "p";
  participant.boot = coherence::ParticipantBootId::from_value(coherence::UInt128{1, 1});
  participant.lifecycle = coherence::ParticipantLifecycle::Active;
  participant.admitted_epoch = image.epoch;
  participant.last_epoch = image.epoch;
  participant.region_count = 1;
  participant.live = true;
  image.participants.emplace(participant.id, participant);

  coherence::ObjectRecord object;
  object.id = coherence::ObjectId::from_value(1);
  object.generation = coherence::ObjectGeneration::from_value(1);
  object.domain = domain_id;
  object.name = "o";
  object.length = 4096;
  object.policy = policy.id;
  object.policy_generation = policy.generation;
  object.lifecycle = coherence::ObjectLifecycle::Active;
  object.ownership_generation = coherence::OwnershipGeneration::from_value(1);
  object.authoritative_version = coherence::VersionId::from_value(1);
  object.published_version = coherence::VersionId::from_value(1);
  object.publication = coherence::PublicationId::from_value(1);
  object.publication_state = coherence::PublicationState::Committed;
  object.replicas.push_back(coherence::RegionId::from_value(1));
  image.objects.emplace(object.id, object);

  coherence::RegionRecord region;
  region.id = coherence::RegionId::from_value(1);
  region.generation = coherence::RegionGeneration::from_value(1);
  region.replica = coherence::ReplicaId::from_value(1);
  region.replica_generation = coherence::ReplicaGeneration::from_value(1);
  region.domain = domain_id;
  region.object = object.id;
  region.object_generation = object.generation;
  region.participant = participant.id;
  region.boot = participant.boot;
  region.memory_domain = coherence::MemoryDomain::HostPageable;
  region.evidence_class = coherence::EvidenceClass::Real;
  region.name = "replica";
  region.length = 4096;
  region.lifecycle = coherence::RegionLifecycle::Active;
  region.state = coherence::CoherenceState::RevalidationRequired;
  region.version = coherence::VersionId::from_value(1);
  region.ownership_generation = object.ownership_generation;
  image.regions.emplace(region.id, region);
  return image;
}

coherence::AuditReport audit_image(const coherence::DurableImage& image) {
  auto store = std::make_shared<coherence::MemoryDurableStore>();
  store->set_snapshot_bytes(coherence::encode_snapshot(image));
  coherence::CoherenceEngine engine;
  const auto attached = engine.attach_store(store);
  if (!attached.has_value()) {
    coherence::AuditReport report;
    report.clean = false;
    coherence::AuditFinding finding;
    finding.code = attached.status().code();
    finding.detail = "recovery refused the image: " + attached.status().to_string();
    report.findings.push_back(finding);
    return report;
  }
  return engine.audit();
}

bool mentions(const coherence::AuditReport& report, const std::string& needle) {
  for (const coherence::AuditFinding& finding : report.findings) {
    if (finding.detail.find(needle) != std::string::npos) return true;
  }
  return false;
}

} // namespace

CF_TEST(a_healthy_runtime_reports_zero_violations) {
  context.phase("SETUP");
  coherence::EngineConfig config;
  config.enable_durability = false;
  coherence::CoherenceEngine engine(config);
  const auto domain = engine.create_domain("healthy");
  const auto object = engine.register_object(domain.value().id, "o", 8192,
                                            coherence::strict_policy("p"));
  const auto alpha = engine.register_participant("alpha", coherence::generate_participant_boot_id(),
                                                 engine.epoch(), "a");
  const auto beta = engine.register_participant("beta", coherence::generate_participant_boot_id(),
                                                engine.epoch(), "b");
  auto make_region = [&](const coherence::ParticipantRecord& participant, const std::string& name,
                         std::uint64_t offset) {
    coherence::RegionRegistration registration;
    registration.context.epoch = engine.epoch();
    registration.context.participant = participant.id;
    registration.context.boot = participant.boot;
    registration.context.object = object.value().id;
    registration.context.object_generation = object.value().generation;
    registration.context.policy_generation = object.value().policy_generation;
    registration.domain = domain.value().id;
    registration.name = name;
    registration.memory_domain = coherence::MemoryDomain::HostPageable;
    registration.evidence_class = coherence::EvidenceClass::Real;
    registration.offset = offset;
    registration.length = 4096;
    return engine.register_region(registration).value();
  };
  const coherence::RegionRecord alpha_region = make_region(alpha.value(), "alpha", 0);
  const coherence::RegionRecord beta_region = make_region(beta.value(), "beta", 4096);

  coherence::AuthorityContext alpha_context;
  alpha_context.epoch = engine.epoch();
  alpha_context.participant = alpha.value().id;
  alpha_context.boot = alpha.value().boot;
  alpha_context.object = object.value().id;
  alpha_context.object_generation = object.value().generation;
  alpha_context.policy_generation = object.value().policy_generation;

  coherence::RevalidateRequest revalidate;
  revalidate.context = alpha_context;
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content.length = 4096;
  revalidate.content.defined = true;
  revalidate.content.crc32c = 1;
  CF_REQUIRE(engine.revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = alpha_context;
  write.region = alpha_region.id;
  write.region_generation = alpha_region.generation;
  const auto grant = engine.acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = alpha_context;
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = alpha_region.id;
  publish.region_generation = alpha_region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = revalidate.content;
  CF_REQUIRE(engine.publish(publish).has_value());

  coherence::AuthorityContext beta_context = alpha_context;
  beta_context.participant = beta.value().id;
  beta_context.boot = beta.value().boot;
  coherence::SyncRequest sync;
  sync.context = beta_context;
  sync.source_region = alpha_region.id;
  sync.source_region_generation = alpha_region.generation;
  sync.destination_region = beta_region.id;
  sync.destination_region_generation = beta_region.generation;
  const auto plan = engine.begin_sync(sync);
  CF_REQUIRE(plan.has_value());
  coherence::SyncCompleteRequest complete;
  complete.context = beta_context;
  complete.operation = plan.value().operation;
  complete.destination_region = beta_region.id;
  complete.destination_region_generation = beta_region.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = revalidate.content;
  CF_REQUIRE(engine.complete_sync(complete).has_value());

  const coherence::AuditReport report = engine.audit();
  CF_EXPECT(report.clean);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(report.findings.size()), 0u);
  CF_EXPECT_EQ_U(report.objects_checked, 1u);
  CF_EXPECT_EQ_U(report.regions_checked, 2u);
  CF_EXPECT_EQ_U(report.participants_checked, 2u);
  CF_EXPECT(report.render().find("result=clean") != std::string::npos);
}

CF_TEST(recovery_recomputes_derived_accounting_from_the_replicas_that_exist) {
  context.phase("SETUP");
  coherence::DurableImage image = base_image();
  // The durable record claims a count that the replicas it describes cannot
  // support. Recovery recomputes derived state instead of trusting it, and the
  // auditor then confirms the recomputed state is consistent.
  image.participants.at(coherence::ParticipantId::from_value(1)).region_count = 99;
  const coherence::AuditReport report = audit_image(image);
  CF_EXPECT(report.clean);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(report.findings.size()), 0u);
}

CF_TEST(the_auditor_detects_a_publication_without_an_authority_identity) {
  context.phase("SETUP");
  coherence::DurableImage image = base_image();
  coherence::ObjectRecord& object = image.objects.at(coherence::ObjectId::from_value(1));
  object.publication = coherence::PublicationId::nil();
  const coherence::AuditReport report = audit_image(image);
  CF_EXPECT(!report.clean);
  CF_EXPECT(mentions(report, "no publication identity"));
}

CF_TEST(the_auditor_detects_a_region_bound_to_the_wrong_object_generation) {
  context.phase("SETUP");
  coherence::DurableImage image = base_image();
  image.regions.at(coherence::RegionId::from_value(1)).object_generation =
      coherence::ObjectGeneration::from_value(7);
  const coherence::AuditReport report = audit_image(image);
  CF_EXPECT(!report.clean);
  CF_EXPECT(mentions(report, "object generation"));
}

CF_TEST(the_auditor_detects_a_replica_claiming_a_future_version) {
  context.phase("SETUP");
  coherence::DurableImage image = base_image();
  image.regions.at(coherence::RegionId::from_value(1)).version =
      coherence::VersionId::from_value(50);
  const coherence::AuditReport report = audit_image(image);
  CF_EXPECT(!report.clean);
  CF_EXPECT(mentions(report, "newer than the authoritative version"));
}

CF_TEST(the_auditor_detects_a_missing_replica_and_a_missing_policy) {
  context.phase("SETUP");
  coherence::DurableImage image = base_image();
  coherence::ObjectRecord& object = image.objects.at(coherence::ObjectId::from_value(1));
  object.replicas.push_back(coherence::RegionId::from_value(999));
  const coherence::AuditReport report = audit_image(image);
  CF_EXPECT(!report.clean);
  CF_EXPECT(mentions(report, "missing replica"));

  context.phase("SECOND");
  coherence::DurableImage no_policy = base_image();
  no_policy.policies.clear();
  const coherence::AuditReport second = audit_image(no_policy);
  CF_EXPECT(!second.clean);
  CF_EXPECT(mentions(second, "missing policy"));
}

CF_TEST(the_auditor_detects_a_published_version_ahead_of_the_authoritative_one) {
  context.phase("SETUP");
  coherence::DurableImage image = base_image();
  image.objects.at(coherence::ObjectId::from_value(1)).published_version =
      coherence::VersionId::from_value(9);
  const coherence::AuditReport report = audit_image(image);
  CF_EXPECT(!report.clean);
  CF_EXPECT(mentions(report, "published version is newer"));
}

CF_TEST(the_auditor_detects_an_unsorted_replica_list) {
  context.phase("SETUP");
  coherence::DurableImage image = base_image();
  coherence::ObjectRecord& object = image.objects.at(coherence::ObjectId::from_value(1));
  object.replicas.push_back(coherence::RegionId::from_value(2));
  object.replicas.push_back(coherence::RegionId::from_value(1));
  const coherence::AuditReport report = audit_image(image);
  CF_EXPECT(!report.clean);
  CF_EXPECT(mentions(report, "canonical order"));
}

CF_TEST(recovery_never_increases_confidence) {
  context.phase("SETUP");
  // Every state that implies dynamic currentness must be downgraded.
  for (const coherence::CoherenceState state :
       {coherence::CoherenceState::Current, coherence::CoherenceState::Stale,
        coherence::CoherenceState::Dirty, coherence::CoherenceState::SyncRequired,
        coherence::CoherenceState::Unknown, coherence::CoherenceState::Invalid}) {
    coherence::DurableImage image = base_image();
    coherence::RegionRecord& region = image.regions.at(coherence::RegionId::from_value(1));
    region.state = state;
    if (state == coherence::CoherenceState::Current) {
      region.evidence = coherence::EvidenceId::from_value(1);
      region.evidence_generation = coherence::EvidenceGeneration::from_value(1);
    }
    auto store = std::make_shared<coherence::MemoryDurableStore>();
    store->set_snapshot_bytes(coherence::encode_snapshot(image));
    coherence::CoherenceEngine engine;
    const auto attached = engine.attach_store(store);
    CF_REQUIRE(attached.has_value());
    const auto restored = engine.get_region(coherence::RegionId::from_value(1));
    CF_REQUIRE(restored.has_value());
    CF_REQUIRE(restored.value().state == coherence::CoherenceState::RevalidationRequired);
  }
  // A retired replica stays retired and is never revived.
  coherence::DurableImage retired_image = base_image();
  coherence::RegionRecord& region = retired_image.regions.at(coherence::RegionId::from_value(1));
  region.state = coherence::CoherenceState::Retired;
  region.lifecycle = coherence::RegionLifecycle::Retired;
  auto store = std::make_shared<coherence::MemoryDurableStore>();
  store->set_snapshot_bytes(coherence::encode_snapshot(retired_image));
  coherence::CoherenceEngine engine;
  CF_REQUIRE(engine.attach_store(store).has_value());
  const auto restored = engine.get_region(coherence::RegionId::from_value(1));
  CF_REQUIRE(restored.has_value());
  CF_REQUIRE(restored.value().state == coherence::CoherenceState::Retired);
  CF_EXPECT(engine.audit().clean);
}

CF_TEST_MAIN()

