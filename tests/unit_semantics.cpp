// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Semantic contract coverage for the coherence engine: authority, versions,
// publication, invalidation, synchronization, dirty state and fail-closed
// behaviour.
#include <memory>
#include <string>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "test_framework.hpp"

namespace {

coherence::ContentFingerprint fingerprint_of(const std::string& text) {
  return coherence::fingerprint_bytes(
      coherence::ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

struct Fixture {
  coherence::EngineConfig config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::ParticipantRecord alpha;
  coherence::ParticipantRecord beta;
  coherence::CoherenceDomainId domain;
  coherence::ObjectId object;

  explicit Fixture(coherence::CoherencePolicy policy = coherence::strict_policy("test")) {
    config.enable_durability = false;
    engine = std::make_unique<coherence::CoherenceEngine>(config);
    auto created_domain = engine->create_domain("test");
    domain = created_domain.value().id;
    auto created_object = engine->register_object(domain, "object", 4096, policy);
    object = created_object.value().id;
    alpha = engine->register_participant("alpha", coherence::generate_participant_boot_id(),
                                         engine->epoch(), "node-a")
                .value();
    beta = engine->register_participant("beta", coherence::generate_participant_boot_id(),
                                        engine->epoch(), "node-b")
               .value();
  }

  coherence::AuthorityContext context(const coherence::ParticipantRecord& participant) {
    coherence::AuthorityContext ctx;
    ctx.epoch = engine->epoch();
    ctx.participant = participant.id;
    ctx.boot = participant.boot;
    ctx.object = object;
    auto record = engine->get_object(object).value();
    ctx.object_generation = record.generation;
    ctx.policy_generation = record.policy_generation;
    ctx.request = coherence::generate_request_id();
    return ctx;
  }

  coherence::RegionRecord region(const coherence::ParticipantRecord& participant,
                                 const std::string& name, std::uint64_t offset,
                                 std::uint64_t length) {
    coherence::RegionRegistration registration;
    registration.context = context(participant);
    registration.domain = domain;
    registration.name = name;
    registration.memory_domain = coherence::MemoryDomain::HostPageable;
    registration.evidence_class = coherence::EvidenceClass::Real;
    registration.offset = offset;
    registration.length = length;
    registration.content = fingerprint_of(name + "-initial");
    return engine->register_region(registration).value();
  }
};

} // namespace

CF_TEST(registration_rejects_duplicate_and_out_of_range_extents) {
  context.phase("SETUP");
  Fixture fixture;
  coherence::RegionRegistration registration;
  registration.context = fixture.context(fixture.alpha);
  registration.domain = fixture.domain;
  registration.name = "replica";
  registration.memory_domain = coherence::MemoryDomain::HostPageable;
  registration.evidence_class = coherence::EvidenceClass::Real;
  registration.offset = 0;
  registration.length = 4096;
  CF_REQUIRE(fixture.engine->register_region(registration).has_value());
  CF_EXPECT(fixture.engine->register_region(registration).code() ==
            coherence::StatusCode::DuplicateIdentity);

  coherence::RegionRegistration outside = registration;
  outside.name = "outside";
  outside.offset = 4000;
  outside.length = 4096;
  CF_EXPECT(fixture.engine->register_region(outside).code() ==
            coherence::StatusCode::InvalidArgument);

  coherence::RegionRegistration overflowing = registration;
  overflowing.name = "overflow";
  overflowing.offset = UINT64_MAX - 4;
  overflowing.length = 16;
  CF_EXPECT(fixture.engine->register_region(overflowing).code() ==
            coherence::StatusCode::InvalidArgument);

  coherence::RegionRegistration duplicate_object = registration;
  duplicate_object.context.object = fixture.object;
  duplicate_object.name = "replica";
  CF_EXPECT(fixture.engine->register_region(duplicate_object).code() ==
            coherence::StatusCode::DuplicateIdentity);
}

CF_TEST(a_new_replica_is_never_current) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord record = fixture.region(fixture.alpha, "replica", 0, 4096);
  CF_EXPECT(record.state != coherence::CoherenceState::Current);
  CF_EXPECT(record.state == coherence::CoherenceState::Unknown ||
            record.state == coherence::CoherenceState::RevalidationRequired);
  CF_EXPECT(!record.evidence.defined());
  // A strict read cannot manufacture currentness from the mere existence of a
  // region.
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  request.region = record.id;
  request.require_current = true;
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().outcome != coherence::ReadOutcome::ReadCurrent);
}

CF_TEST(a_stale_participant_boot_cannot_act) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  request.context.boot = coherence::ParticipantBootId::from_value(coherence::UInt128{7, 7});
  request.region = region.id;
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().reason == coherence::StatusCode::StaleBoot);
  CF_EXPECT(decision.value().outcome == coherence::ReadOutcome::ReadBlocked);
}

CF_TEST(a_stale_coordinator_epoch_cannot_act) {
  context.phase("SETUP");
  Fixture fixture;
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  request.context.epoch = coherence::CoordinatorEpoch::from_value(999);
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().reason == coherence::StatusCode::StaleEpoch);
  CF_EXPECT(decision.value().outcome == coherence::ReadOutcome::ReadBlocked);
}

CF_TEST(a_stale_object_generation_cannot_act) {
  context.phase("SETUP");
  Fixture fixture;
  coherence::WriteRequest request;
  request.context = fixture.context(fixture.alpha);
  request.context.object_generation = coherence::ObjectGeneration::from_value(42);
  const auto grant = fixture.engine->acquire_write(request);
  // A write request that cannot even be resolved is a failure, not a grant.
  CF_EXPECT(!grant.has_value());
  CF_EXPECT(grant.status().code() == coherence::StatusCode::StaleObjectGeneration);
}

CF_TEST(a_stale_policy_generation_cannot_act) {
  context.phase("SETUP");
  Fixture fixture;
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  request.context.policy_generation = coherence::PolicyGeneration::from_value(99);
  const auto decision = fixture.engine->acquire_read(request);
  CF_EXPECT(decision.has_value());
  CF_EXPECT(decision.value().reason == coherence::StatusCode::StalePolicy);
}

CF_TEST(revalidation_establishes_initial_currentness_from_evidence) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest request;
  request.context = fixture.context(fixture.alpha);
  request.region = region.id;
  request.region_generation = region.generation;
  request.observed_version = coherence::VersionId::from_value(1);
  request.content = fingerprint_of("contents");
  const auto updated = fixture.engine->revalidate_region(request);
  CF_REQUIRE(updated.has_value());
  CF_EXPECT(updated.value().state == coherence::CoherenceState::Current);
  CF_EXPECT(updated.value().evidence.defined());
  CF_EXPECT(updated.value().version == coherence::VersionId::from_value(1));

  // Revalidation without a content fingerprint is refused where the policy
  // requires one.
  coherence::RevalidateRequest bare = request;
  bare.content = coherence::ContentFingerprint{};
  CF_EXPECT(fixture.engine->revalidate_region(bare).code() ==
            coherence::StatusCode::EvidenceMissing);
}

CF_TEST(only_one_writer_may_hold_authority_for_a_generation) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord alpha_region = fixture.region(fixture.alpha, "alpha", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("alpha");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());

  coherence::WriteRequest alpha_write;
  alpha_write.context = fixture.context(fixture.alpha);
  alpha_write.region = alpha_region.id;
  alpha_write.region_generation = alpha_region.generation;
  const auto first = fixture.engine->acquire_write(alpha_write);
  CF_REQUIRE(first.has_value());
  CF_EXPECT(first.value().granted);
  CF_EXPECT(first.value().may_mutate_now);

  const coherence::RegionRecord beta_region = fixture.region(fixture.beta, "beta", 0, 4096);
  coherence::WriteRequest beta_write;
  beta_write.context = fixture.context(fixture.beta);
  beta_write.region = beta_region.id;
  beta_write.region_generation = beta_region.generation;
  const auto second = fixture.engine->acquire_write(beta_write);
  CF_REQUIRE(second.has_value());
  CF_EXPECT(!second.value().granted);
  CF_EXPECT(second.value().reason == coherence::StatusCode::ExclusiveWriterConflict);
  const auto audit = fixture.engine->audit();
  CF_EXPECT(audit.clean);
}

CF_TEST(publication_requires_authority_and_a_precise_base_version) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("alpha");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());

  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);

  // Wrong ownership generation: rejected.
  coherence::PublishRequest wrong_ownership;
  wrong_ownership.context = fixture.context(fixture.alpha);
  wrong_ownership.ownership_generation =
      coherence::OwnershipGeneration::from_value(grant.value().context.ownership_generation.value() + 5);
  wrong_ownership.region = region.id;
  wrong_ownership.region_generation = region.generation;
  wrong_ownership.expected_base_version = coherence::VersionId::nil();
  wrong_ownership.allow_without_dirty = true;
  wrong_ownership.content = fingerprint_of("alpha");
  auto rejected = fixture.engine->publish(wrong_ownership);
  CF_REQUIRE(rejected.has_value());
  CF_EXPECT(rejected.value().state == coherence::PublicationState::None);
  CF_EXPECT(rejected.value().reason == coherence::StatusCode::StaleOwnership);

  // Wrong base version: rejected.
  coherence::PublishRequest wrong_base = wrong_ownership;
  wrong_base.ownership_generation = grant.value().context.ownership_generation;
  wrong_base.expected_base_version = coherence::VersionId::from_value(9);
  rejected = fixture.engine->publish(wrong_base);
  CF_REQUIRE(rejected.has_value());
  CF_EXPECT(rejected.value().reason == coherence::StatusCode::StalePublication);

  // A write by a participant that does not own the region is refused.
  coherence::PublishRequest foreign = wrong_base;
  foreign.context = fixture.context(fixture.beta);
  foreign.expected_base_version = coherence::VersionId::nil();
  rejected = fixture.engine->publish(foreign);
  CF_REQUIRE(rejected.has_value());
  CF_EXPECT(rejected.value().state == coherence::PublicationState::None);
  CF_EXPECT(rejected.value().reason == coherence::StatusCode::NotWriteAuthorized);
}

CF_TEST(publication_is_not_implicit_in_a_write) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("alpha");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  CF_REQUIRE(fixture.engine->acquire_write(write).has_value());

  const auto object = fixture.engine->get_object(fixture.object);
  CF_REQUIRE(object.has_value());
  CF_EXPECT(!object.value().authoritative_version.defined());
  CF_EXPECT(!object.value().publication.defined());

  coherence::DirtyRequest dirty;
  dirty.context = fixture.context(fixture.alpha);
  dirty.region = region.id;
  dirty.region_generation = region.generation;
  dirty.ownership_generation = object.value().ownership_generation;
  dirty.base_version = coherence::VersionId::nil();
  const auto marked = fixture.engine->mark_dirty(dirty);
  CF_REQUIRE(marked.has_value());
  CF_EXPECT(marked.value() == coherence::DirtyCondition::DirtyUnpublished);
  // Dirty is not published.
  const auto after_dirty = fixture.engine->get_object(fixture.object);
  CF_EXPECT(!after_dirty.value().authoritative_version.defined());
  CF_EXPECT(after_dirty.value().has_unpublished_dirty);
}

CF_TEST(initial_publication_and_version_progression) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("one");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);

  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region.id;
  publish.region_generation = region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("one");
  const auto first = fixture.engine->publish(publish);
  CF_REQUIRE(first.has_value());
  CF_EXPECT(first.value().state == coherence::PublicationState::Committed);
  CF_EXPECT(first.value().version == coherence::VersionId::from_value(1));

  // A retry with the same request identity returns the same version.
  const auto retry = fixture.engine->publish(publish);
  CF_REQUIRE(retry.has_value());
  CF_EXPECT(retry.value().idempotent_replay);
  CF_EXPECT(retry.value().version == first.value().version);
  CF_EXPECT(retry.value().publication == first.value().publication);

  // A new request identity against the old base version is refused.
  coherence::PublishRequest stale = publish;
  stale.context.request = coherence::generate_request_id();
  const auto rejected = fixture.engine->publish(stale);
  CF_REQUIRE(rejected.has_value());
  CF_EXPECT(rejected.value().reason == coherence::StatusCode::StalePublication);

  const auto object = fixture.engine->get_object(fixture.object);
  CF_EXPECT(object.value().authoritative_version == coherence::VersionId::from_value(1));
  CF_EXPECT(object.value().publication == first.value().publication);
  const auto published_region = fixture.engine->get_region(region.id);
  CF_EXPECT(published_region.value().state == coherence::CoherenceState::Current);
  CF_EXPECT(published_region.value().version == coherence::VersionId::from_value(1));
  CF_EXPECT(fixture.engine->audit().clean);
}

CF_TEST(publication_requires_a_content_fingerprint_under_strict_policy) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("alpha");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value());

  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region.id;
  publish.region_generation = region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = coherence::ContentFingerprint{};
  const auto receipt = fixture.engine->publish(publish);
  CF_REQUIRE(receipt.has_value());
  CF_EXPECT(receipt.value().state == coherence::PublicationState::None);
  CF_EXPECT(receipt.value().reason == coherence::StatusCode::EvidenceMissing);
}

CF_TEST(write_transfer_requires_acknowledged_invalidations) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord alpha_region = fixture.region(fixture.alpha, "alpha", 0, 4096);
  const coherence::RegionRecord beta_region = fixture.region(fixture.beta, "beta", 2048, 2048);

  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("alpha");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = alpha_region.id;
  write.region_generation = alpha_region.generation;
  const auto alpha_grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(alpha_grant.has_value() && alpha_grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = alpha_grant.value().context.ownership_generation;
  publish.region = alpha_region.id;
  publish.region_generation = alpha_region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("alpha");
  CF_REQUIRE(fixture.engine->publish(publish).has_value());
  // The first writer releases its authority explicitly before another writer
  // can take it, which is what advances the ownership generation.
  coherence::ReleaseRequest handback;
  handback.context = fixture.context(fixture.alpha);
  handback.ownership_generation = alpha_grant.value().context.ownership_generation;
  handback.release_write_authority = true;
  CF_REQUIRE(fixture.engine->release(handback).has_value());

  // Beta synchronizes so that it holds a current replica too.
  coherence::SyncRequest sync;
  sync.context = fixture.context(fixture.beta);
  sync.source_region = alpha_region.id;
  sync.source_region_generation = alpha_region.generation;
  sync.destination_region = beta_region.id;
  sync.destination_region_generation = beta_region.generation;
  const auto plan = fixture.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());
  CF_EXPECT(plan.value().state == coherence::SyncState::Requested);
  coherence::SyncCompleteRequest complete;
  complete.context = fixture.context(fixture.beta);
  complete.operation = plan.value().operation;
  complete.destination_region = beta_region.id;
  complete.destination_region_generation = beta_region.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = plan.value().plan.expected_content;
  const auto completed = fixture.engine->complete_sync(complete);
  CF_REQUIRE(completed.has_value());
  CF_EXPECT(completed.value().state == coherence::SyncState::Completed);

  // Beta now takes write authority. Alpha must be invalidated and must
  // acknowledge before Beta may mutate.
  coherence::WriteRequest beta_write;
  beta_write.context = fixture.context(fixture.beta);
  beta_write.region = beta_region.id;
  beta_write.region_generation = beta_region.generation;
  const auto beta_grant = fixture.engine->acquire_write(beta_write);
  CF_REQUIRE(beta_grant.has_value());
  CF_EXPECT(beta_grant.value().granted);
  CF_EXPECT(!beta_grant.value().may_mutate_now);
  CF_REQUIRE(beta_grant.value().required_invalidations.size() == 1);
  const coherence::InvalidationRecord invalidation = beta_grant.value().required_invalidations.front();
  CF_EXPECT(invalidation.region == alpha_region.id);

  coherence::PublishRequest early;
  early.context = fixture.context(fixture.beta);
  early.ownership_generation = beta_grant.value().context.ownership_generation;
  early.region = beta_region.id;
  early.region_generation = beta_region.generation;
  early.expected_base_version = fixture.engine->get_object(fixture.object).value().authoritative_version;
  early.allow_without_dirty = true;
  early.content = fingerprint_of("beta");
  const auto blocked = fixture.engine->publish(early);
  CF_REQUIRE(blocked.has_value());
  CF_EXPECT(blocked.value().reason == coherence::StatusCode::InvalidationOutstanding);

  coherence::InvalidationAck ack;
  ack.context = fixture.context(fixture.alpha);
  ack.invalidation = invalidation.id;
  ack.region = alpha_region.id;
  ack.region_generation = alpha_region.generation;
  ack.replica_generation = alpha_region.replica_generation;
  ack.superseded_version = alpha_region.version;
  const auto acknowledged = fixture.engine->acknowledge_invalidation(ack);
  CF_REQUIRE(acknowledged.has_value());
  CF_EXPECT(acknowledged.value().state == coherence::InvalidationState::Acknowledged);
  // A duplicate acknowledgement is idempotent.
  const auto duplicate = fixture.engine->acknowledge_invalidation(ack);
  CF_REQUIRE(duplicate.has_value());
  CF_EXPECT(duplicate.value().idempotent_replay);

  const auto alpha_after = fixture.engine->get_region(alpha_region.id);
  CF_EXPECT(alpha_after.value().state != coherence::CoherenceState::Current);
  const auto effective = fixture.engine->acquire_write(beta_write);
  CF_REQUIRE(effective.has_value());
  CF_EXPECT(effective.value().may_mutate_now);
  CF_EXPECT(fixture.engine->audit().clean);
}

CF_TEST(invalidation_is_generation_bound_and_never_hits_a_replacement) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("alpha");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());

  // Retire the replica and register a replacement.
  CF_REQUIRE(fixture.engine->retire_region(region.id, region.generation, fixture.engine->epoch()).ok());
  const coherence::RegionRecord replacement = fixture.region(fixture.alpha, "replacement", 0, 4096);

  coherence::InvalidationRequest invalidation;
  invalidation.context.object = fixture.object;
  invalidation.context.epoch = fixture.engine->epoch();
  invalidation.context.object_generation =
      fixture.engine->get_object(fixture.object).value().generation;
  invalidation.ownership_generation =
      fixture.engine->get_object(fixture.object).value().ownership_generation;
  invalidation.target_region = region.id;
  invalidation.target_region_generation = region.generation;
  invalidation.target_replica_generation = region.replica_generation;
  const auto outcome = fixture.engine->invalidate(invalidation);
  // The retired replica is inert, so the invalidation is satisfied vacuously
  // and is never applied to the replacement.
  CF_REQUIRE(outcome.has_value());
  CF_EXPECT(outcome.value().state == coherence::InvalidationState::Superseded);
  const auto untouched = fixture.engine->get_region(replacement.id);
  CF_EXPECT(untouched.value().state != coherence::CoherenceState::Fenced);
  CF_EXPECT(untouched.value().state != coherence::CoherenceState::Stale);
  CF_EXPECT_AUDIT_CLEAN(fixture.engine->audit());
}

CF_TEST(synchronization_completion_verifies_content) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord alpha_region = fixture.region(fixture.alpha, "alpha", 0, 4096);
  const coherence::RegionRecord beta_region = fixture.region(fixture.beta, "beta", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("published-bytes");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = alpha_region.id;
  write.region_generation = alpha_region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = alpha_region.id;
  publish.region_generation = alpha_region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("published-bytes");
  CF_REQUIRE(fixture.engine->publish(publish).has_value());

  coherence::SyncRequest sync;
  sync.context = fixture.context(fixture.beta);
  sync.source_region = alpha_region.id;
  sync.source_region_generation = alpha_region.generation;
  sync.destination_region = beta_region.id;
  sync.destination_region_generation = beta_region.generation;
  const auto plan = fixture.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());

  // A completion whose observed content does not match the publication is
  // refused, and the replica does not become current.
  coherence::SyncCompleteRequest wrong = {};
  wrong.context = fixture.context(fixture.beta);
  wrong.operation = plan.value().operation;
  wrong.destination_region = beta_region.id;
  wrong.destination_region_generation = beta_region.generation;
  wrong.destination_new_version = plan.value().plan.source_version;
  wrong.observed_content = fingerprint_of("something-else");
  const auto mismatched = fixture.engine->complete_sync(wrong);
  CF_REQUIRE(mismatched.has_value());
  CF_EXPECT(mismatched.value().state == coherence::SyncState::Failed);
  CF_EXPECT(mismatched.value().reason == coherence::StatusCode::ContentMismatch);
  const auto destination = fixture.engine->get_region(beta_region.id);
  CF_EXPECT(destination.value().state != coherence::CoherenceState::Current);
  CF_EXPECT(destination.value().state == coherence::CoherenceState::RevalidationRequired);
}

CF_TEST(a_stale_synchronization_completion_cannot_certify_currentness) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord alpha_region = fixture.region(fixture.alpha, "alpha", 0, 4096);
  const coherence::RegionRecord beta_region = fixture.region(fixture.beta, "beta", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = alpha_region.id;
  write.region_generation = alpha_region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = alpha_region.id;
  publish.region_generation = alpha_region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("v1");
  const auto first = fixture.engine->publish(publish);
  CF_REQUIRE(first.has_value());

  coherence::SyncRequest sync;
  sync.context = fixture.context(fixture.beta);
  sync.source_region = alpha_region.id;
  sync.source_region_generation = alpha_region.generation;
  sync.destination_region = beta_region.id;
  sync.destination_region_generation = beta_region.generation;
  const auto plan = fixture.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());

  // Publish a second version before the transfer completes.
  coherence::DirtyRequest dirty;
  dirty.context = fixture.context(fixture.alpha);
  dirty.region = alpha_region.id;
  dirty.region_generation = alpha_region.generation;
  dirty.ownership_generation = fixture.engine->get_object(fixture.object).value().ownership_generation;
  dirty.base_version = first.value().version;
  CF_REQUIRE(fixture.engine->mark_dirty(dirty).has_value());
  coherence::PublishRequest second = publish;
  second.context.request = coherence::generate_request_id();
  second.expected_base_version = first.value().version;
  second.allow_without_dirty = false;
  second.content = fingerprint_of("v2");
  const auto published = fixture.engine->publish(second);
  CF_REQUIRE(published.has_value());
  CF_EXPECT(published.value().version == first.value().version.next());

  coherence::SyncCompleteRequest stale;
  stale.context = fixture.context(fixture.beta);
  stale.operation = plan.value().operation;
  stale.destination_region = beta_region.id;
  stale.destination_region_generation = beta_region.generation;
  stale.destination_new_version = plan.value().plan.source_version;
  stale.observed_content = plan.value().plan.expected_content;
  const auto outcome = fixture.engine->complete_sync(stale);
  CF_REQUIRE(outcome.has_value());
  CF_EXPECT(outcome.value().state == coherence::SyncState::Failed);
  CF_EXPECT(outcome.value().reason == coherence::StatusCode::StalePublication);
  const auto destination = fixture.engine->get_region(beta_region.id);
  CF_EXPECT(destination.value().state != coherence::CoherenceState::Current);
  CF_EXPECT(fixture.engine->audit().clean);
}

CF_TEST(duplicate_synchronization_completion_is_idempotent) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord alpha_region = fixture.region(fixture.alpha, "alpha", 0, 4096);
  const coherence::RegionRecord beta_region = fixture.region(fixture.beta, "beta", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = alpha_region.id;
  write.region_generation = alpha_region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = alpha_region.id;
  publish.region_generation = alpha_region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->publish(publish).has_value());
  coherence::SyncRequest sync;
  sync.context = fixture.context(fixture.beta);
  sync.source_region = alpha_region.id;
  sync.source_region_generation = alpha_region.generation;
  sync.destination_region = beta_region.id;
  sync.destination_region_generation = beta_region.generation;
  const auto plan = fixture.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());
  coherence::SyncCompleteRequest complete;
  complete.context = fixture.context(fixture.beta);
  complete.operation = plan.value().operation;
  complete.destination_region = beta_region.id;
  complete.destination_region_generation = beta_region.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = plan.value().plan.expected_content;
  CF_REQUIRE(fixture.engine->complete_sync(complete).has_value());
  const auto duplicate = fixture.engine->complete_sync(complete);
  CF_REQUIRE(duplicate.has_value());
  CF_EXPECT(duplicate.value().idempotent_replay);
  CF_EXPECT(duplicate.value().state == coherence::SyncState::Completed);
}

CF_TEST(synchronization_outcome_unknown_is_reported_honestly) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord alpha_region = fixture.region(fixture.alpha, "alpha", 0, 4096);
  const coherence::RegionRecord beta_region = fixture.region(fixture.beta, "beta", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = alpha_region.id;
  write.region_generation = alpha_region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = alpha_region.id;
  publish.region_generation = alpha_region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->publish(publish).has_value());
  coherence::SyncRequest sync;
  sync.context = fixture.context(fixture.beta);
  sync.source_region = alpha_region.id;
  sync.source_region_generation = alpha_region.generation;
  sync.destination_region = beta_region.id;
  sync.destination_region_generation = beta_region.generation;
  const auto plan = fixture.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());
  coherence::SyncFailRequest failure;
  failure.context = fixture.context(fixture.beta);
  failure.operation = plan.value().operation;
  failure.cause = coherence::StatusCode::TransportFailure;
  failure.outcome_unknown = true;
  const auto outcome = fixture.engine->fail_sync(failure);
  CF_REQUIRE(outcome.has_value());
  CF_EXPECT(outcome.value().state == coherence::SyncState::OutcomeUnknown);
  const auto destination = fixture.engine->get_region(beta_region.id);
  CF_EXPECT(destination.value().state == coherence::CoherenceState::SyncRequired);
  CF_EXPECT(destination.value().state != coherence::CoherenceState::Current);
}

CF_TEST(releasing_write_authority_with_unpublished_dirty_never_invents_success) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region.id;
  publish.region_generation = region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("v1");
  const auto first = fixture.engine->publish(publish);
  CF_REQUIRE(first.has_value());

  coherence::DirtyRequest dirty;
  dirty.context = fixture.context(fixture.alpha);
  dirty.region = region.id;
  dirty.region_generation = region.generation;
  dirty.ownership_generation = fixture.engine->get_object(fixture.object).value().ownership_generation;
  dirty.base_version = first.value().version;
  CF_REQUIRE(fixture.engine->mark_dirty(dirty).has_value());

  coherence::ReleaseRequest release;
  release.context = fixture.context(fixture.alpha);
  release.ownership_generation = fixture.engine->get_object(fixture.object).value().ownership_generation;
  release.release_write_authority = true;
  const auto released = fixture.engine->release(release);
  CF_REQUIRE(released.has_value());

  const auto object = fixture.engine->get_object(fixture.object);
  CF_REQUIRE(object.has_value());
  // The strict policy uses RequireRecovery: the object demands explicit
  // resolution rather than pretending the modification was published.
  CF_EXPECT(object.value().lifecycle == coherence::ObjectLifecycle::RecoveryRequired);
  CF_EXPECT(object.value().dirty_condition == coherence::DirtyCondition::DirtyUnknown);
  CF_EXPECT(object.value().authoritative_version == first.value().version);
  CF_EXPECT(!object.value().has_unpublished_dirty);
  CF_EXPECT(fixture.engine->audit().clean);
}

CF_TEST(fencing_a_participant_revokes_authority_and_marks_replicas) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);

  const auto fenced =
      fixture.engine->fence_participant(fixture.alpha.id, fixture.engine->epoch(), "test fence");
  CF_REQUIRE(fenced.has_value());
  CF_EXPECT(fenced.value().lifecycle == coherence::ParticipantLifecycle::Fenced);
  const auto object = fixture.engine->get_object(fixture.object);
  CF_EXPECT(!object.value().writer.defined());
  CF_EXPECT(object.value().authority == coherence::AuthorityMode::None);
  const auto region_after = fixture.engine->get_region(region.id);
  CF_EXPECT(region_after.value().state == coherence::CoherenceState::Fenced);
  // Repeated fencing is idempotent.
  const auto again =
      fixture.engine->fence_participant(fixture.alpha.id, fixture.engine->epoch(), "again");
  CF_REQUIRE(again.has_value());
  CF_EXPECT(again.value().lifecycle == coherence::ParticipantLifecycle::Fenced);

  // The old boot identity can no longer act.
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().reason == coherence::StatusCode::Fenced);

  // Re-admission under a fresh boot creates a distinct incarnation.
  const auto readmitted = fixture.engine->register_participant(
      "alpha", coherence::generate_participant_boot_id(), fixture.engine->epoch(), "node-a");
  CF_REQUIRE(readmitted.has_value());
  CF_EXPECT(readmitted.value().id == fixture.alpha.id);
  CF_EXPECT(readmitted.value().boot != fixture.alpha.boot);
  CF_EXPECT(readmitted.value().previous_boot == fixture.alpha.boot);
  CF_EXPECT(fixture.engine->audit().clean);
}

CF_TEST(region_and_object_retirement_revoke_live_authority) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  CF_REQUIRE(fixture.engine->acquire_write(write).has_value());

  CF_REQUIRE(fixture.engine->retire_region(region.id, region.generation, fixture.engine->epoch()).ok());
  const auto retired_region = fixture.engine->get_region(region.id);
  CF_EXPECT(retired_region.value().lifecycle == coherence::RegionLifecycle::Retired);
  CF_EXPECT(retired_region.value().state == coherence::CoherenceState::Retired);

  CF_REQUIRE(fixture.engine->retire_object(fixture.object,
                                           fixture.engine->get_object(fixture.object).value().generation,
                                           fixture.engine->epoch())
                 .ok());
  const auto retired_object = fixture.engine->get_object(fixture.object);
  CF_EXPECT(retired_object.value().lifecycle == coherence::ObjectLifecycle::Retired);
  CF_EXPECT(!retired_object.value().writer.defined());
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().reason == coherence::StatusCode::Retired);
  CF_EXPECT(fixture.engine->audit().clean);
}

CF_TEST(retired_generations_reject_later_transitions) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  CF_REQUIRE(fixture.engine->retire_region(region.id, region.generation, fixture.engine->epoch()).ok());
  // Retiring again with the same generation is idempotent.
  CF_EXPECT(fixture.engine->retire_region(region.id, region.generation, fixture.engine->epoch()).ok());
  // A stale region generation is rejected.
  CF_EXPECT(fixture.engine->retire_region(region.id, coherence::RegionGeneration::from_value(9),
                                          fixture.engine->epoch())
                .code() == coherence::StatusCode::StaleRegionGeneration);
  // A stale epoch is rejected.
  CF_EXPECT(fixture.engine->retire_region(region.id, region.generation,
                                          coherence::CoordinatorEpoch::from_value(77))
                .code() == coherence::StatusCode::StaleEpoch);
}

CF_TEST(policy_change_advances_the_policy_generation_and_stales_decisions) {
  context.phase("SETUP");
  Fixture fixture;
  const auto object_before = fixture.engine->get_object(fixture.object);
  coherence::CoherencePolicy next = coherence::eventual_policy(4, "relaxed");
  const auto updated = fixture.engine->set_policy(
      fixture.object, object_before.value().generation, object_before.value().policy_generation, next);
  CF_REQUIRE(updated.has_value());
  CF_EXPECT(updated.value().policy_generation.value() ==
            object_before.value().policy_generation.value() + 1);
  // A decision taken under the previous policy generation is refused.
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  request.context.policy_generation = object_before.value().policy_generation;
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().reason == coherence::StatusCode::StalePolicy);
  // Replaying the same policy change with the old generation is refused.
  CF_EXPECT(fixture.engine->set_policy(fixture.object, object_before.value().generation,
                                       object_before.value().policy_generation, next)
                .status()
                .code() == coherence::StatusCode::StalePolicy);
}

CF_TEST(eventual_policy_reports_latest_known_and_authoritative_separately) {
  context.phase("SETUP");
  Fixture fixture{coherence::eventual_policy(4, "eventual-test")};
  const coherence::RegionRecord alpha_region = fixture.region(fixture.alpha, "alpha", 0, 4096);
  const coherence::RegionRecord beta_region = fixture.region(fixture.beta, "beta", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = alpha_region.id;
  revalidate.region_generation = alpha_region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = alpha_region.id;
  write.region_generation = alpha_region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context(fixture.alpha);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = alpha_region.id;
  publish.region_generation = alpha_region.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fingerprint_of("v1");
  const auto receipt = fixture.engine->publish(publish);
  CF_REQUIRE(receipt.has_value());

  // Beta never synchronized. Under an eventual policy the read may proceed but
  // must report both versions distinctly.
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.beta);
  request.region = beta_region.id;
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().outcome == coherence::ReadOutcome::ReadCurrent ||
            decision.value().outcome == coherence::ReadOutcome::ReadStaleAllowed ||
            decision.value().outcome == coherence::ReadOutcome::ReadAfterSync);
  if (decision.value().outcome == coherence::ReadOutcome::ReadStaleAllowed) {
    CF_EXPECT(decision.value().stale_allowed);
    CF_EXPECT(decision.value().region_version != decision.value().authoritative_version);
  }
}

CF_TEST(strict_policy_fails_closed_when_no_current_replica_exists) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::ReadRequest request;
  request.context = fixture.context(fixture.alpha);
  request.region = region.id;
  request.require_current = true;
  const auto decision = fixture.engine->acquire_read(request);
  CF_REQUIRE(decision.has_value());
  // There is no current source replica and no evidence, so the read is blocked.
  CF_EXPECT(decision.value().outcome == coherence::ReadOutcome::ReadBlocked);
  CF_EXPECT(decision.value().outcome != coherence::ReadOutcome::ReadCurrent);
  CF_EXPECT(!decision.value().rationale.empty());
}

CF_TEST(a_read_of_an_unsupported_backend_is_reported_as_unsupported) {
  context.phase("SETUP");
  Fixture fixture;
  coherence::RegionRegistration registration;
  registration.context = fixture.context(fixture.alpha);
  registration.domain = fixture.domain;
  registration.name = "remote";
  registration.memory_domain = coherence::MemoryDomain::Remote;
  // The backend declares that it cannot produce evidence at all.
  registration.evidence_class = coherence::EvidenceClass::Unsupported;
  registration.offset = 0;
  registration.length = 512;
  const auto rejected = fixture.engine->register_region(registration);
  CF_EXPECT(!rejected.has_value());
  CF_EXPECT(rejected.status().code() == coherence::StatusCode::Unsupported);
}

CF_TEST(read_only_policy_refuses_write_authority) {
  context.phase("SETUP");
  coherence::CoherencePolicy read_only = coherence::strict_policy("read-only");
  read_only.write_ownership = coherence::WriteOwnershipMode::ReadOnlyObject;
  Fixture fixture{read_only};
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value());
  CF_EXPECT(!grant.value().granted);
  CF_EXPECT(grant.value().reason == coherence::StatusCode::NotWriteAuthorized);
}

CF_TEST(mark_dirty_requires_ownership_and_current_contents) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::DirtyRequest dirty;
  dirty.context = fixture.context(fixture.alpha);
  dirty.region = region.id;
  dirty.region_generation = region.generation;
  dirty.ownership_generation = coherence::OwnershipGeneration::from_value(1);
  dirty.base_version = coherence::VersionId::nil();
  // No write authority has been granted.
  const auto refused = fixture.engine->mark_dirty(dirty);
  CF_EXPECT(!refused.has_value());
  CF_EXPECT(refused.status().code() == coherence::StatusCode::NotWriteAuthorized);

  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);

  // A stale ownership generation is rejected.
  coherence::DirtyRequest stale = dirty;
  stale.ownership_generation = coherence::OwnershipGeneration::from_value(999);
  const auto rejected = fixture.engine->mark_dirty(stale);
  CF_EXPECT(!rejected.has_value());
  CF_EXPECT(rejected.status().code() == coherence::StatusCode::StaleOwnership);

  // A stale base version is rejected.
  coherence::DirtyRequest wrong_base = dirty;
  wrong_base.ownership_generation = grant.value().context.ownership_generation;
  wrong_base.base_version = coherence::VersionId::from_value(5);
  CF_EXPECT(fixture.engine->mark_dirty(wrong_base).status().code() ==
            coherence::StatusCode::StalePublication);
}

CF_TEST(shutdown_revokes_authority_and_is_idempotent) {
  context.phase("SETUP");
  Fixture fixture;
  const coherence::RegionRecord region = fixture.region(fixture.alpha, "replica", 0, 4096);
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context(fixture.alpha);
  revalidate.region = region.id;
  revalidate.region_generation = region.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context(fixture.alpha);
  write.region = region.id;
  write.region_generation = region.generation;
  CF_REQUIRE(fixture.engine->acquire_write(write).has_value());

  CF_REQUIRE(fixture.engine->begin_shutdown(fixture.engine->epoch()).ok());
  CF_REQUIRE(fixture.engine->begin_shutdown(fixture.engine->epoch()).ok());
  const auto object = fixture.engine->get_object(fixture.object);
  CF_EXPECT(!object.value().writer.defined());
  CF_EXPECT(object.value().authority == coherence::AuthorityMode::None);

  // New authority is refused.
  coherence::WriteRequest after;
  after.context = fixture.context(fixture.beta);
  after.region = region.id;
  after.region_generation = region.generation;
  CF_EXPECT(fixture.engine->acquire_write(after).status().code() ==
            coherence::StatusCode::ShuttingDown);
  CF_REQUIRE(fixture.engine->complete_shutdown(fixture.engine->epoch()).ok());
  CF_REQUIRE(fixture.engine->complete_shutdown(fixture.engine->epoch()).ok());
}

CF_TEST(decision_log_is_bounded_and_deterministic) {
  context.phase("SETUP");
  coherence::EngineConfig config;
  config.enable_durability = false;
  config.max_decision_log = 8;
  coherence::CoherenceEngine engine(config);
  const auto domain = engine.create_domain("log");
  const auto object = engine.register_object(domain.value().id, "o", 64,
                                             coherence::strict_policy("p"));
  const auto participant = engine.register_participant(
      "p", coherence::generate_participant_boot_id(), engine.epoch(), "");
  for (int i = 0; i < 50; ++i) {
    coherence::ReadRequest request;
    request.context.epoch = engine.epoch();
    request.context.participant = participant.value().id;
    request.context.boot = participant.value().boot;
    request.context.object = object.value().id;
    request.context.object_generation = object.value().generation;
    request.context.policy_generation = object.value().policy_generation;
    (void)engine.acquire_read(request);
  }
  const std::string first = engine.render_decision_log();
  const std::string second = engine.render_decision_log();
  CF_EXPECT_EQ(first, second);
  CF_EXPECT(first.find("entries=8") != std::string::npos);
}

CF_TEST(snapshot_rendering_is_deterministic_and_ordered) {
  context.phase("SETUP");
  Fixture fixture;
  fixture.region(fixture.alpha, "a-replica", 0, 2048);
  fixture.region(fixture.beta, "b-replica", 2048, 2048);
  const auto first = fixture.engine->snapshot(coherence::SnapshotOptions{});
  const auto second = fixture.engine->snapshot(coherence::SnapshotOptions{});
  CF_REQUIRE(first.has_value() && second.has_value());
  CF_EXPECT_EQ(first.value().render(), second.value().render());
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(first.value().regions.size()), 2u);
  // Region ordering follows identity, not insertion or map iteration order.
  CF_EXPECT(first.value().regions.front().id < first.value().regions.back().id);
}

CF_TEST(recovery_from_a_durable_store_does_not_restore_currentness) {
  context.phase("SETUP");
  coherence::EngineConfig config;
  coherence::CoherenceEngine writer(config);
  auto store = std::make_shared<coherence::MemoryDurableStore>();
  CF_REQUIRE(writer.attach_store(store).has_value());
  const auto domain = writer.create_domain("durable");
  const auto object = writer.register_object(domain.value().id, "o", 4096,
                                            coherence::strict_policy("p"));
  const auto participant = writer.register_participant(
      "p", coherence::generate_participant_boot_id(), writer.epoch(), "");
  coherence::RegionRegistration registration;
  registration.context.epoch = writer.epoch();
  registration.context.participant = participant.value().id;
  registration.context.boot = participant.value().boot;
  registration.context.object = object.value().id;
  registration.context.object_generation = object.value().generation;
  registration.context.policy_generation = object.value().policy_generation;
  registration.domain = domain.value().id;
  registration.name = "replica";
  registration.memory_domain = coherence::MemoryDomain::HostPageable;
  registration.evidence_class = coherence::EvidenceClass::Real;
  registration.offset = 0;
  registration.length = 4096;
  const auto region = writer.register_region(registration);
  CF_REQUIRE(region.has_value());
  coherence::RevalidateRequest revalidate;
  revalidate.context = registration.context;
  revalidate.region = region.value().id;
  revalidate.region_generation = region.value().generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fingerprint_of("v1");
  CF_REQUIRE(writer.revalidate_region(revalidate).has_value());
  CF_EXPECT(writer.get_region(region.value().id).value().state ==
            coherence::CoherenceState::Current);
  (void)writer.begin_shutdown(writer.epoch());

  context.phase("RECOVER");
  coherence::CoherenceEngine reader(config);
  auto recovered = reader.attach_store(store);
  CF_REQUIRE(recovered.has_value());
  CF_EXPECT(recovered.value().performed);
  CF_EXPECT(recovered.value().new_epoch.value() > recovered.value().previous_epoch.value());
  CF_EXPECT(recovered.value().regions_downgraded >= 1);
  const auto restored = reader.get_region(region.value().id);
  CF_REQUIRE(restored.has_value());
  CF_EXPECT(restored.value().state == coherence::CoherenceState::RevalidationRequired);
  CF_EXPECT(restored.value().state != coherence::CoherenceState::Current);
  // The persisted evidence is provenance only and cannot be treated as proof.
  CF_EXPECT(restored.value().evidence.defined());
  const auto restored_object = reader.get_object(object.value().id);
  CF_REQUIRE(restored_object.has_value());
  CF_EXPECT(restored_object.value().authority == coherence::AuthorityMode::RevalidationRequired ||
            restored_object.value().authority == coherence::AuthorityMode::None);
  CF_EXPECT(!restored_object.value().writer.defined());
  // A request carrying the pre-restart epoch is refused.
  coherence::ReadRequest stale;
  stale.context = registration.context;
  const auto decision = reader.acquire_read(stale);
  // A stale epoch is reported as an explicit, machine-readable rejection with a
  // blocked read decision, never as a guessed outcome.
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().reason == coherence::StatusCode::StaleEpoch);
  CF_EXPECT(decision.value().outcome == coherence::ReadOutcome::ReadBlocked);
  CF_EXPECT_AUDIT_CLEAN(reader.audit());
}

CF_TEST_MAIN()

