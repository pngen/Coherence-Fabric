// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Adversarial coverage: stale authority, replays, conflicting grants,
// malformed input, integer boundaries and ambiguous outcomes.
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "coherence/protocol.hpp"
#include "coherence/transport.hpp"
#include "process_util.hpp"
#include "test_framework.hpp"

namespace {

struct AdversarialWorld {
  coherence::EngineConfig config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::CoherenceDomainId domain;
  coherence::ObjectId object;
  coherence::ParticipantRecord writer;
  coherence::ParticipantRecord reader;
  coherence::RegionRecord writer_region;
  coherence::RegionRecord reader_region;

  AdversarialWorld(bool durable = false) {
    config.enable_durability = durable;
    engine = std::make_unique<coherence::CoherenceEngine>(config);
    if (durable) {
      (void)engine->attach_store(std::make_shared<coherence::MemoryDurableStore>());
    }
    domain = engine->create_domain("adversarial").value().id;
    object = engine->register_object(domain, "object", 4096, coherence::strict_policy("adv"))
                 .value().id;
    writer = engine->register_participant("writer", coherence::generate_participant_boot_id(),
                                          engine->epoch(), "")
                 .value();
    reader = engine->register_participant("reader", coherence::generate_participant_boot_id(),
                                          engine->epoch(), "")
                 .value();
    writer_region = add_region(writer, "writer-replica", 0);
    reader_region = add_region(reader, "reader-replica", 2048);
  }

  coherence::RegionRecord add_region(const coherence::ParticipantRecord& participant,
                                     const std::string& name, std::uint64_t offset) {
    coherence::RegionRegistration registration;
    registration.context = context(participant);
    registration.domain = domain;
    registration.name = name;
    registration.memory_domain = coherence::MemoryDomain::HostPageable;
    registration.evidence_class = coherence::EvidenceClass::Real;
    registration.offset = offset;
    registration.length = 2048;
    return engine->register_region(registration).value();
  }

  coherence::AuthorityContext context(const coherence::ParticipantRecord& participant) {
    coherence::AuthorityContext ctx;
    ctx.epoch = engine->epoch();
    ctx.participant = participant.id;
    ctx.boot = participant.boot;
    ctx.object = object;
    const auto record = engine->get_object(object).value();
    ctx.object_generation = record.generation;
    ctx.policy_generation = record.policy_generation;
    ctx.request = coherence::generate_request_id();
    return ctx;
  }

  bool publish_initial(const coherence::ContentFingerprint& content) {
    coherence::RevalidateRequest revalidate;
    revalidate.context = context(writer);
    revalidate.region = writer_region.id;
    revalidate.region_generation = writer_region.generation;
    revalidate.observed_version = coherence::VersionId::from_value(1);
    revalidate.content = content;
    if (!engine->revalidate_region(revalidate).has_value()) return false;
    coherence::WriteRequest write;
    write.context = context(writer);
    write.region = writer_region.id;
    write.region_generation = writer_region.generation;
    const auto grant = engine->acquire_write(write);
    if (!grant.has_value() || !grant.value().granted) return false;
    coherence::PublishRequest publish;
    publish.context = context(writer);
    publish.ownership_generation = grant.value().context.ownership_generation;
    publish.region = writer_region.id;
    publish.region_generation = writer_region.generation;
    publish.expected_base_version = coherence::VersionId::nil();
    publish.allow_without_dirty = true;
    publish.content = content;
    const auto receipt = engine->publish(publish);
    return receipt.has_value() &&
           receipt.value().state == coherence::PublicationState::Committed;
  }
};

coherence::ContentFingerprint content_of(std::uint32_t crc) {
  coherence::ContentFingerprint fingerprint;
  fingerprint.defined = true;
  fingerprint.crc32c = crc;
  fingerprint.length = 2048;
  return fingerprint;
}

} // namespace

CF_TEST(a_replayed_publication_request_never_creates_a_second_version) {
  context.phase("SETUP");
  AdversarialWorld world(true);
  CF_REQUIRE(world.publish_initial(content_of(1)));
  const auto object = world.engine->get_object(world.object);
  const coherence::VersionId version = object.value().authoritative_version;

  coherence::RevalidateRequest revalidate;
  revalidate.context = world.context(world.writer);
  revalidate.region = world.writer_region.id;
  revalidate.region_generation = world.writer_region.generation;
  revalidate.observed_version = version;
  revalidate.content = content_of(2);
  CF_REQUIRE(world.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = world.context(world.writer);
  write.region = world.writer_region.id;
  write.region_generation = world.writer_region.generation;
  const auto grant = world.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().may_mutate_now);
  coherence::DirtyRequest dirty;
  dirty.context = world.context(world.writer);
  dirty.region = world.writer_region.id;
  dirty.region_generation = world.writer_region.generation;
  dirty.ownership_generation = grant.value().context.ownership_generation;
  dirty.base_version = version;
  CF_REQUIRE(world.engine->mark_dirty(dirty).has_value());

  context.phase("REPLAY");
  coherence::PublishRequest publish;
  publish.context = world.context(world.writer);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = world.writer_region.id;
  publish.region_generation = world.writer_region.generation;
  publish.expected_base_version = version;
  publish.content = content_of(3);
  const auto first = world.engine->publish(publish);
  CF_REQUIRE(first.has_value());
  CF_REQUIRE(first.value().state == coherence::PublicationState::Committed);
  for (int i = 0; i < 5; ++i) {
    const auto replay = world.engine->publish(publish);
    CF_REQUIRE(replay.has_value());
    CF_EXPECT(replay.value().idempotent_replay);
    CF_EXPECT(replay.value().version == first.value().version);
  }
  const auto after = world.engine->get_object(world.object);
  CF_EXPECT(after.value().authoritative_version == first.value().version);
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(a_stale_region_generation_cannot_be_invalidated_or_synchronized) {
  context.phase("SETUP");
  AdversarialWorld world;
  CF_REQUIRE(world.publish_initial(content_of(1)));

  coherence::InvalidationRequest invalidation;
  invalidation.context.object = world.object;
  invalidation.context.epoch = world.engine->epoch();
  invalidation.context.object_generation = world.engine->get_object(world.object).value().generation;
  invalidation.ownership_generation =
      world.engine->get_object(world.object).value().ownership_generation;
  invalidation.target_region = world.reader_region.id;
  invalidation.target_region_generation = coherence::RegionGeneration::from_value(77);
  invalidation.target_replica_generation = world.reader_region.replica_generation;
  const auto rejected = world.engine->invalidate(invalidation);
  CF_EXPECT(!rejected.has_value());
  CF_EXPECT(rejected.status().code() == coherence::StatusCode::StaleRegionGeneration);

  coherence::SyncRequest sync;
  sync.context = world.context(world.reader);
  sync.source_region = world.writer_region.id;
  sync.source_region_generation = coherence::RegionGeneration::from_value(99);
  sync.destination_region = world.reader_region.id;
  sync.destination_region_generation = world.reader_region.generation;
  const auto rejected_sync = world.engine->begin_sync(sync);
  CF_EXPECT(!rejected_sync.has_value());
}

CF_TEST(a_stale_synchronization_completion_from_the_wrong_boot_is_refused) {
  context.phase("SETUP");
  AdversarialWorld world;
  CF_REQUIRE(world.publish_initial(content_of(1)));
  coherence::SyncRequest sync;
  sync.context = world.context(world.reader);
  sync.source_region = world.writer_region.id;
  sync.source_region_generation = world.writer_region.generation;
  sync.destination_region = world.reader_region.id;
  sync.destination_region_generation = world.reader_region.generation;
  const auto plan = world.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());

  coherence::SyncCompleteRequest complete;
  complete.context = world.context(world.reader);
  complete.context.boot = coherence::ParticipantBootId::from_value(coherence::UInt128{9, 9});
  complete.operation = plan.value().operation;
  complete.destination_region = world.reader_region.id;
  complete.destination_region_generation = world.reader_region.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = plan.value().plan.expected_content;
  CF_EXPECT(!world.engine->complete_sync(complete).has_value());
  // The plan is still open and the replica is still not current.
  CF_EXPECT(world.engine->get_region(world.reader_region.id).value().state !=
            coherence::CoherenceState::Current);
}

CF_TEST(a_region_retired_before_a_synchronization_completes_cannot_become_current) {
  context.phase("SETUP");
  AdversarialWorld world;
  CF_REQUIRE(world.publish_initial(content_of(1)));
  coherence::SyncRequest sync;
  sync.context = world.context(world.reader);
  sync.source_region = world.writer_region.id;
  sync.source_region_generation = world.writer_region.generation;
  sync.destination_region = world.reader_region.id;
  sync.destination_region_generation = world.reader_region.generation;
  const auto plan = world.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());
  CF_REQUIRE(world.engine->retire_region(world.reader_region.id, world.reader_region.generation,
                                         world.engine->epoch())
                 .ok());
  coherence::SyncCompleteRequest complete;
  complete.context = world.context(world.reader);
  complete.operation = plan.value().operation;
  complete.destination_region = world.reader_region.id;
  complete.destination_region_generation = world.reader_region.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = plan.value().plan.expected_content;
  const auto outcome = world.engine->complete_sync(complete);
  CF_REQUIRE(outcome.has_value());
  CF_EXPECT(outcome.value().state == coherence::SyncState::Cancelled);
  CF_EXPECT(outcome.value().reason == coherence::StatusCode::Retired);
  const auto region = world.engine->get_region(world.reader_region.id);
  CF_REQUIRE(region.has_value());
  CF_EXPECT(region.value().state == coherence::CoherenceState::Retired);
  CF_EXPECT(region.value().state != coherence::CoherenceState::Current);
  CF_EXPECT_AUDIT_CLEAN(world.engine->audit());
}

CF_TEST(an_impossible_count_in_a_frame_body_is_rejected) {
  context.phase("SETUP");
  // A body that declares four billion replicas must be rejected before any
  // allocation is attempted.
  coherence::ByteWriter writer;
  writer.u16(coherence::kRecordVersion);
  writer.strong_id(coherence::ObjectId::from_value(1));
  writer.strong_id(coherence::ObjectGeneration::from_value(1));
  writer.strong_id(coherence::CoherenceDomainId::from_value(1));
  writer.text("object");
  writer.u64(4096);
  writer.strong_id(coherence::PolicyId::from_value(1));
  writer.strong_id(coherence::PolicyGeneration::from_value(1));
  writer.u8(1);  // lifecycle
  writer.u8(0);  // authority
  writer.strong_id(coherence::ParticipantId::nil());
  writer.strong_id(coherence::ParticipantBootId::nil());
  writer.strong_id(coherence::OwnershipGeneration::from_value(1));
  writer.strong_id(coherence::VersionId::nil());
  writer.strong_id(coherence::VersionId::nil());
  writer.strong_id(coherence::PublicationId::nil());
  writer.strong_id(coherence::OperationSequence::nil());
  writer.strong_id(coherence::PublicationId::nil());
  writer.strong_id(coherence::VersionId::nil());
  writer.u8(0);
  coherence::encode_fingerprint(writer, coherence::ContentFingerprint{});
  writer.u64(0xFFFFFFFFu);  // impossible replica count
  coherence::ObjectRecord decoded;
  coherence::ByteReader reader(writer.span());
  CF_EXPECT(!coherence::decode_object(reader, decoded, coherence::default_decode_limits()));
}

CF_TEST(integer_boundaries_are_checked_before_use) {
  context.phase("SETUP");
  AdversarialWorld world;
  coherence::RegionRegistration registration;
  registration.context = world.context(world.writer);
  registration.domain = world.domain;
  registration.name = "boundary";
  registration.memory_domain = coherence::MemoryDomain::HostPageable;
  registration.evidence_class = coherence::EvidenceClass::Real;
  registration.offset = UINT64_MAX;
  registration.length = UINT64_MAX;
  CF_EXPECT(world.engine->register_region(registration).code() ==
            coherence::StatusCode::InvalidArgument);

  registration.offset = 0;
  registration.length = 0;
  CF_EXPECT(world.engine->register_region(registration).code() ==
            coherence::StatusCode::InvalidArgument);

  registration.length = UINT64_MAX;
  CF_EXPECT(world.engine->register_region(registration).code() ==
            coherence::StatusCode::InvalidArgument);

  const auto object = world.engine->register_object(
      world.domain, "huge", UINT64_MAX, coherence::strict_policy("p"));
  CF_EXPECT(!object.has_value());
  CF_EXPECT(object.status().code() == coherence::StatusCode::InvalidArgument);
}

CF_TEST(capacity_bounds_are_enforced) {
  context.phase("SETUP");
  coherence::EngineConfig config;
  config.enable_durability = false;
  config.max_objects = 2;
  config.max_regions = 3;
  config.max_participants = 2;
  coherence::CoherenceEngine engine(config);
  const auto domain = engine.create_domain("bounded");
  CF_REQUIRE(engine.register_object(domain.value().id, "a", 64, coherence::strict_policy("p"))
                 .has_value());
  CF_REQUIRE(engine.register_object(domain.value().id, "b", 64, coherence::strict_policy("p"))
                 .has_value());
  CF_EXPECT(engine.register_object(domain.value().id, "c", 64, coherence::strict_policy("p"))
                .code() == coherence::StatusCode::CapacityExceeded);
  CF_REQUIRE(engine.register_participant("p1", coherence::generate_participant_boot_id(),
                                         engine.epoch(), "")
                 .has_value());
  CF_REQUIRE(engine.register_participant("p2", coherence::generate_participant_boot_id(),
                                         engine.epoch(), "")
                 .has_value());
  CF_EXPECT(engine.register_participant("p3", coherence::generate_participant_boot_id(),
                                        engine.epoch(), "")
                .code() == coherence::StatusCode::CapacityExceeded);
}

CF_TEST(a_participant_that_reconnects_with_an_old_boot_cannot_mutate) {
  context.phase("SETUP");
  AdversarialWorld world;
  CF_REQUIRE(world.publish_initial(content_of(1)));
  coherence::WriteRequest write;
  write.context = world.context(world.writer);
  write.region = world.writer_region.id;
  write.region_generation = world.writer_region.generation;
  CF_REQUIRE(world.engine->acquire_write(write).value().granted);

  context.phase("KILL");
  // Simulate the participant dying: the coordinator fences the incarnation.
  CF_REQUIRE(world.engine->fence_participant(world.writer.id, world.engine->epoch(),
                                             "process death")
                 .has_value());

  context.phase("RESTART_SAME_BOOT");
  // A process that comes back believing it still owns its old boot identity is
  // re-admitted as the same incarnation, which is fenced, so it cannot act.
  const auto same =
      world.engine->register_participant("writer", world.writer.boot, world.engine->epoch(), "");
  CF_REQUIRE(same.has_value());
  CF_EXPECT(same.value().id == world.writer.id);
  CF_EXPECT(same.value().lifecycle == coherence::ParticipantLifecycle::Fenced);
  const auto reused = world.engine->acquire_write(write);
  CF_EXPECT(!reused.has_value());
  CF_EXPECT(reused.status().code() == coherence::StatusCode::Fenced);

  context.phase("RESTART_NEW_BOOT");
  const auto fresh = world.engine->register_participant(
      "writer", coherence::generate_participant_boot_id(), world.engine->epoch(), "");
  CF_REQUIRE(fresh.has_value());
  CF_EXPECT(fresh.value().id == world.writer.id);
  CF_EXPECT(fresh.value().boot != world.writer.boot);
  CF_EXPECT(fresh.value().lifecycle == coherence::ParticipantLifecycle::Active);
  CF_EXPECT(fresh.value().previous_boot == world.writer.boot);
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(repeated_shutdown_and_fence_are_idempotent_under_repetition) {
  context.phase("SETUP");
  AdversarialWorld world;
  for (int i = 0; i < 50; ++i) {
    CF_REQUIRE(world.engine->fence_participant(world.writer.id, world.engine->epoch(), "repeat")
                   .has_value());
  }
  CF_REQUIRE(world.engine->begin_shutdown(world.engine->epoch()).ok());
  for (int i = 0; i < 50; ++i) {
    CF_REQUIRE(world.engine->begin_shutdown(world.engine->epoch()).ok());
    CF_REQUIRE(world.engine->complete_shutdown(world.engine->epoch()).ok());
  }
}

CF_TEST(a_participant_that_dies_while_dirty_never_produces_a_silent_publication) {
  context.phase("SETUP");
  AdversarialWorld world(true);
  CF_REQUIRE(world.publish_initial(content_of(1)));
  const coherence::VersionId version =
      world.engine->get_object(world.object).value().authoritative_version;
  coherence::RevalidateRequest revalidate;
  revalidate.context = world.context(world.writer);
  revalidate.region = world.writer_region.id;
  revalidate.region_generation = world.writer_region.generation;
  revalidate.observed_version = version;
  revalidate.content = content_of(2);
  CF_REQUIRE(world.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = world.context(world.writer);
  write.region = world.writer_region.id;
  write.region_generation = world.writer_region.generation;
  const auto grant = world.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().may_mutate_now);
  coherence::DirtyRequest dirty;
  dirty.context = world.context(world.writer);
  dirty.region = world.writer_region.id;
  dirty.region_generation = world.writer_region.generation;
  dirty.ownership_generation = grant.value().context.ownership_generation;
  dirty.base_version = version;
  CF_REQUIRE(world.engine->mark_dirty(dirty).has_value());

  context.phase("DIE");
  CF_REQUIRE(world.engine->fence_participant(world.writer.id, world.engine->epoch(),
                                             "died while dirty")
                 .has_value());
  const auto object = world.engine->get_object(world.object);
  CF_REQUIRE(object.has_value());
  // No publication was invented, and the loss is reported rather than hidden.
  CF_EXPECT(object.value().authoritative_version == version);
  CF_EXPECT(object.value().dirty_condition == coherence::DirtyCondition::DirtyUnknown ||
            object.value().dirty_condition == coherence::DirtyCondition::DirtyLost);
  CF_EXPECT(object.value().lifecycle == coherence::ObjectLifecycle::RecoveryRequired ||
            object.value().lifecycle == coherence::ObjectLifecycle::Active);
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(publication_durable_but_unanswered_does_not_produce_a_second_version_on_retry) {
  context.phase("SETUP");
  AdversarialWorld world(true);
  CF_REQUIRE(world.publish_initial(content_of(1)));
  const coherence::VersionId version =
      world.engine->get_object(world.object).value().authoritative_version;
  coherence::RevalidateRequest revalidate;
  revalidate.context = world.context(world.writer);
  revalidate.region = world.writer_region.id;
  revalidate.region_generation = world.writer_region.generation;
  revalidate.observed_version = version;
  revalidate.content = content_of(2);
  CF_REQUIRE(world.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = world.context(world.writer);
  write.region = world.writer_region.id;
  write.region_generation = world.writer_region.generation;
  const auto grant = world.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().may_mutate_now);
  coherence::DirtyRequest dirty;
  dirty.context = world.context(world.writer);
  dirty.region = world.writer_region.id;
  dirty.region_generation = world.writer_region.generation;
  dirty.ownership_generation = grant.value().context.ownership_generation;
  dirty.base_version = version;
  CF_REQUIRE(world.engine->mark_dirty(dirty).has_value());

  coherence::PublishRequest publish;
  publish.context = world.context(world.writer);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = world.writer_region.id;
  publish.region_generation = world.writer_region.generation;
  publish.expected_base_version = version;
  publish.content = content_of(3);
  const auto committed = world.engine->publish(publish);
  CF_REQUIRE(committed.has_value());
  CF_REQUIRE(committed.value().state == coherence::PublicationState::Committed);
  const coherence::VersionId published = committed.value().version;

  context.phase("RECOVER");
  // A coordinator restart must not turn a retried request into a second
  // version. The committed request identity is durable, so the restarted
  // coordinator answers with the original publication identity.
  coherence::EngineConfig config;
  config.enable_durability = true;
  coherence::CoherenceEngine first(config);
  auto store = std::make_shared<coherence::MemoryDurableStore>();
  CF_REQUIRE(first.attach_store(store).has_value());
  const auto domain = first.create_domain("replay");
  const auto object = first.register_object(domain.value().id, "o", 2048,
                                           coherence::strict_policy("p"));
  const auto member = first.register_participant("p", coherence::generate_participant_boot_id(),
                                                 first.epoch(), "");
  coherence::RegionRegistration registration;
  registration.context.epoch = first.epoch();
  registration.context.participant = member.value().id;
  registration.context.boot = member.value().boot;
  registration.context.object = object.value().id;
  registration.context.object_generation = object.value().generation;
  registration.context.policy_generation = object.value().policy_generation;
  registration.context.request = coherence::generate_request_id();
  registration.domain = domain.value().id;
  registration.name = "replica";
  registration.memory_domain = coherence::MemoryDomain::HostPageable;
  registration.evidence_class = coherence::EvidenceClass::Real;
  registration.offset = 0;
  registration.length = 2048;
  const auto region = first.register_region(registration);
  CF_REQUIRE(region.has_value());
  coherence::RevalidateRequest initial;
  initial.context = registration.context;
  initial.region = region.value().id;
  initial.region_generation = region.value().generation;
  initial.observed_version = coherence::VersionId::from_value(1);
  initial.content = content_of(1);
  CF_REQUIRE(first.revalidate_region(initial).has_value());
  coherence::WriteRequest acquire;
  acquire.context = registration.context;
  acquire.region = region.value().id;
  acquire.region_generation = region.value().generation;
  const auto acquired = first.acquire_write(acquire);
  CF_REQUIRE(acquired.has_value() && acquired.value().may_mutate_now);
  coherence::PublishRequest request;
  request.context = registration.context;
  request.ownership_generation = acquired.value().context.ownership_generation;
  request.region = region.value().id;
  request.region_generation = region.value().generation;
  request.expected_base_version = coherence::VersionId::nil();
  request.allow_without_dirty = true;
  request.content = content_of(2);
  const auto receipt = first.publish(request);
  CF_REQUIRE(receipt.has_value());
  CF_REQUIRE(receipt.value().state == coherence::PublicationState::Committed);
  const coherence::VersionId restarted_version = receipt.value().version;

  context.phase("RESTART");
  coherence::CoherenceEngine second(config);
  const auto recovered = second.attach_store(store);
  CF_REQUIRE(recovered.has_value());
  // The request identity is durable, so the retry is answered identically and
  // no second version is created.
  coherence::PublishRequest retry = request;
  retry.context.epoch = second.epoch();
  const auto replayed = second.publish(retry);
  CF_REQUIRE(replayed.has_value());
  CF_EXPECT(replayed.value().idempotent_replay);
  CF_EXPECT(replayed.value().version == restarted_version);
  CF_EXPECT(replayed.value().publication == receipt.value().publication);
  const auto object_after = second.get_object(object.value().id);
  CF_REQUIRE(object_after.has_value());
  CF_EXPECT(object_after.value().authoritative_version == version);
  CF_EXPECT(second.audit().clean);
}

#if defined(COHERENCE_FABRIC_ASAN)
CF_TEST(genuine_asan_instrumentation_is_present) {
  context.phase("SETUP");
  // The build supplies the probe's path as a compile definition; the
  // environment variable is an override for manual runs.
#if defined(COHERENCE_FABRIC_ASAN_PROBE)
  std::string probe = COHERENCE_FABRIC_ASAN_PROBE;
#else
  std::string probe;
#endif
  if (probe.empty()) probe = cfproc::environment_value("COHERENCE_FABRIC_ASAN_PROBE");
  if (probe.empty()) {
    // Fall back to the probe shipped beside this executable, which is where the
    // build places it.
    const std::filesystem::path beside = cfproc::executable_path("cf_asan_probe");
    if (std::filesystem::exists(beside)) probe = beside.string();
  }
  CF_REQUIRE(!probe.empty());
  const std::filesystem::path path = probe;
  CF_REQUIRE(std::filesystem::exists(path));

  context.phase("PROBE");
  // The probe deliberately performs an out-of-bounds write. A genuine
  // sanitizer must detect it and terminate the process; an uninstrumented probe
  // would exit successfully.
  auto child = cfproc::Child::spawn(path, {"--trigger"});
  CF_REQUIRE(child != nullptr);
  child->close_stdin();
  const int code = child->wait();
  child->drain();
  bool reported = false;
  for (const std::string& line : child->transcript()) {
    if (line.find("AddressSanitizer") != std::string::npos) reported = true;
  }
  child->close();
  CF_EXPECT(code != 0);
  CF_EXPECT(reported);
}
#endif

CF_TEST_MAIN()

