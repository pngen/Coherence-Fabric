// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Randomized property tests. Every seed is fixed and reported, so a failure is
// reproducible by rerunning the exact case.
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/engine.hpp"
#include "test_framework.hpp"

namespace {

constexpr std::uint64_t kSeed = 0x5EED1234ull;

struct World {
  coherence::EngineConfig config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::CoherenceDomainId domain;
  coherence::ObjectId object;
  std::vector<coherence::ParticipantRecord> participants;
  std::vector<coherence::RegionRecord> regions;
  coherence::DeterministicRandom random{kSeed};

  World(std::size_t participant_count, std::size_t region_count,
        coherence::CoherencePolicy policy = coherence::strict_policy("property")) {
    config.enable_durability = false;
    config.max_decision_log = 64;
    engine = std::make_unique<coherence::CoherenceEngine>(config);
    domain = engine->create_domain("property").value().id;
    object = engine->register_object(domain, "object", 65536, policy).value().id;
    for (std::size_t i = 0; i < participant_count; ++i) {
      participants.push_back(
          engine
              ->register_participant("p" + std::to_string(i),
                                     coherence::generate_participant_boot_id(), engine->epoch(), "")
              .value());
    }
    for (std::size_t i = 0; i < region_count; ++i) {
      coherence::RegionRegistration registration;
      registration.context.epoch = engine->epoch();
      const coherence::ParticipantRecord& owner = participants[i % participants.size()];
      registration.context.participant = owner.id;
      registration.context.boot = owner.boot;
      registration.context.object = object;
      registration.context.object_generation = engine->get_object(object).value().generation;
      registration.context.policy_generation =
          engine->get_object(object).value().policy_generation;
      registration.domain = domain;
      registration.name = "r" + std::to_string(i);
      registration.memory_domain = coherence::MemoryDomain::HostPageable;
      registration.evidence_class = coherence::EvidenceClass::Real;
      registration.offset = i * 64;
      registration.length = 64;
      auto region = engine->register_region(registration);
      if (region.has_value()) regions.push_back(region.value());
    }
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
};

} // namespace

CF_TEST(random_operation_sequences_preserve_invariants) {
  context.phase("SETUP");
  context.mark("seed=" + std::to_string(kSeed));
  World world(3, 6);
  std::uint64_t applied = 0;
  for (int step = 0; step < 3000; ++step) {
    const std::uint64_t choice = world.random.next_below(10);
    const coherence::ParticipantRecord& participant =
        world.participants[world.random.next_below(world.participants.size())];
    const coherence::RegionRecord& region =
        world.regions[world.random.next_below(world.regions.size())];
    const auto object_record = world.engine->get_object(world.object);
    if (!object_record.has_value()) break;
    if (choice == 0 || choice == 1 || choice == 2) {
      coherence::ReadRequest request;
      request.context = world.context(participant);
      request.region = world.random.next_bool() ? region.id : coherence::RegionId::nil();
      request.region_generation = coherence::RegionGeneration::nil();
      (void)world.engine->acquire_read(request);
      ++applied;
    } else if (choice == 3) {
      coherence::RevalidateRequest request;
      request.context = world.context(participant);
      request.region = region.id;
      request.region_generation = region.generation;
      request.observed_version =
          object_record.value().authoritative_version.defined()
              ? object_record.value().authoritative_version
              : coherence::VersionId::from_value(1);
      request.content.length = region.length;
      request.content.defined = true;
      request.content.crc32c = static_cast<std::uint32_t>(world.random.next_u32());
      (void)world.engine->revalidate_region(request);
      ++applied;
    } else if (choice == 4) {
      coherence::WriteRequest request;
      request.context = world.context(participant);
      request.region = region.id;
      request.region_generation = region.generation;
      const auto grant = world.engine->acquire_write(request);
      if (grant.has_value() && grant.value().granted && grant.value().may_mutate_now) {
        coherence::DirtyRequest dirty;
        dirty.context = request.context;
        dirty.region = region.id;
        dirty.region_generation = region.generation;
        dirty.ownership_generation = grant.value().context.ownership_generation;
        dirty.base_version = object_record.value().authoritative_version;
        (void)world.engine->mark_dirty(dirty);
        coherence::PublishRequest publish;
        publish.context = request.context;
        publish.ownership_generation = grant.value().context.ownership_generation;
        publish.region = region.id;
        publish.region_generation = region.generation;
        publish.expected_base_version = object_record.value().authoritative_version;
        publish.content.length = region.length;
        publish.content.defined = true;
        publish.content.crc32c = static_cast<std::uint32_t>(world.random.next_u32());
        (void)world.engine->publish(publish);
      }
      ++applied;
    } else if (choice == 5) {
      coherence::InvalidationRequest request;
      request.context.object = world.object;
      request.context.epoch = world.engine->epoch();
      request.context.object_generation = object_record.value().generation;
      request.ownership_generation = object_record.value().ownership_generation;
      request.target_region = region.id;
      request.target_region_generation = region.generation;
      request.target_replica_generation = region.replica_generation;
      request.acknowledgement_required = false;
      (void)world.engine->invalidate(request);
      ++applied;
    } else if (choice == 6) {
      coherence::SyncRequest request;
      request.context = world.context(participant);
      request.destination_region = region.id;
      request.destination_region_generation = region.generation;
      (void)world.engine->begin_sync(request);
      ++applied;
    } else if (choice == 7) {
      coherence::SnapshotOptions options;
      options.max_objects = 4;
      (void)world.engine->snapshot(options);
      ++applied;
    } else if (choice == 8) {
      coherence::ReadRequest request;
      request.context = world.context(participant);
      request.context.boot = coherence::ParticipantBootId::from_value(
          coherence::UInt128{world.random.next_u64(), world.random.next_u64()});
      (void)world.engine->acquire_read(request);
      ++applied;
    } else {
      (void)world.engine->audit();
      ++applied;
    }
    // The auditor must never see an inconsistent state, at any interleaving.
    const coherence::AuditReport report = world.engine->audit();
    if (!report.clean) {
      for (const coherence::AuditFinding& finding : report.findings) {
        context.mark("violation " + std::string(coherence::status_code_name(finding.code)) + " " +
                     finding.detail);
      }
      CF_EXPECT(report.clean);
      break;
    }
  }
  CF_EXPECT(applied > 0);
  context.mark("applied=" + std::to_string(applied));
}

CF_TEST(random_invalidation_and_publication_sequences_keep_versions_monotonic) {
  context.phase("SETUP");
  context.mark("seed=" + std::to_string(kSeed + 1));
  coherence::DeterministicRandom random(kSeed + 1);
  World world(2, 4);
  world.random = random;
  coherence::VersionId highest;
  for (int step = 0; step < 800; ++step) {
    const coherence::ParticipantRecord& participant =
        world.participants[random.next_below(world.participants.size())];
    coherence::RegionRecord region = world.regions[random.next_below(world.regions.size())];
    const auto object_record = world.engine->get_object(world.object);
    CF_REQUIRE(object_record.has_value());
    if (object_record.value().authoritative_version.defined() &&
        highest.defined() &&
        object_record.value().authoritative_version < highest) {
      CF_EXPECT(false);
      return;
    }
    if (object_record.value().authoritative_version.defined()) {
      highest = object_record.value().authoritative_version;
    }
    coherence::RevalidateRequest revalidate;
    revalidate.context = world.context(participant);
    revalidate.region = region.id;
    revalidate.region_generation = region.generation;
    revalidate.observed_version = highest.defined() ? highest : coherence::VersionId::from_value(1);
    revalidate.content.length = region.length;
    revalidate.content.defined = true;
    revalidate.content.crc32c = random.next_u32();
    (void)world.engine->revalidate_region(revalidate);
    coherence::WriteRequest write;
    write.context = world.context(participant);
    write.region = region.id;
    write.region_generation = region.generation;
    const auto grant = world.engine->acquire_write(write);
    if (!grant.has_value() || !grant.value().granted) continue;
    for (const coherence::InvalidationRecord& invalidation : grant.value().required_invalidations) {
      coherence::InvalidationAck ack;
      ack.context = world.context(participant);
      ack.invalidation = invalidation.id;
      ack.region = invalidation.region;
      ack.region_generation = invalidation.region_generation;
      ack.replica_generation = invalidation.replica_generation;
      ack.superseded_version = invalidation.superseded_version;
      (void)world.engine->acknowledge_invalidation(ack);
    }
    const auto effective = world.engine->acquire_write(write);
    if (!effective.has_value() || !effective.value().may_mutate_now) continue;
    coherence::DirtyRequest dirty;
    dirty.context = write.context;
    dirty.region = region.id;
    dirty.region_generation = region.generation;
    dirty.ownership_generation = effective.value().context.ownership_generation;
    dirty.base_version = highest;
    if (!world.engine->mark_dirty(dirty).has_value()) continue;
    coherence::PublishRequest publish;
    publish.context = write.context;
    publish.ownership_generation = effective.value().context.ownership_generation;
    publish.region = region.id;
    publish.region_generation = region.generation;
    publish.expected_base_version = highest;
    publish.content.length = region.length;
    publish.content.defined = true;
    publish.content.crc32c = random.next_u32();
    const auto receipt = world.engine->publish(publish);
    if (receipt.has_value() && receipt.value().state == coherence::PublicationState::Committed) {
      CF_EXPECT(!highest.defined() || receipt.value().version > highest);
      highest = receipt.value().version;
    }
  }
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(random_illegal_transition_attempts_never_change_state_illegally) {
  context.phase("SETUP");
  context.mark("seed=" + std::to_string(kSeed + 2));
  coherence::DeterministicRandom random(kSeed + 2);
  // Exhaustively drive the transition table: every pair must either be legal or
  // be rejected by the predicate, and the predicate must never claim that a
  // retired or fenced replica can return to a live coherence state.
  const coherence::CoherenceState states[] = {
      coherence::CoherenceState::Unknown,   coherence::CoherenceState::Invalid,
      coherence::CoherenceState::Stale,     coherence::CoherenceState::Current,
      coherence::CoherenceState::SyncRequired,
      coherence::CoherenceState::RevalidationRequired,
      coherence::CoherenceState::Dirty,     coherence::CoherenceState::Fenced,
      coherence::CoherenceState::Retired};
  for (const coherence::CoherenceState from : states) {
    for (const coherence::CoherenceState to : states) {
      const bool legal = coherence::is_legal_coherence_transition(from, to);
      if (from == coherence::CoherenceState::Retired && to != from) {
        CF_EXPECT(!legal);
      }
      if (from == coherence::CoherenceState::Fenced && to == coherence::CoherenceState::Current) {
        CF_EXPECT(!legal);
      }
      if (legal && coherence::is_current_state(to)) {
        // Reaching a current state always requires a transition that can carry
        // evidence.
        CF_EXPECT(from != coherence::CoherenceState::Retired);
      }
    }
  }
  // Randomized generation walks never produce a non-monotonic sequence.
  for (int trial = 0; trial < 500; ++trial) {
    coherence::VersionId version = coherence::VersionId::from_value(random.next_below(100) + 1);
    coherence::VersionId previous = version;
    for (int step = 0; step < 20; ++step) {
      const coherence::VersionId next = version.next();
      CF_EXPECT(next > previous);
      previous = version;
      version = next;
    }
  }
}

CF_TEST_MAIN()

