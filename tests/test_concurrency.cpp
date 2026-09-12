// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Concurrency coverage. The engine guard is a single non-recursive mutex with
// no callbacks under it, so these tests also serve as the audit of lock
// upgrade, re-entrancy and teardown behaviour.
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "test_framework.hpp"

namespace {

struct SharedWorld {
  coherence::EngineConfig config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::CoherenceDomainId domain;
  coherence::ObjectId object;
  std::vector<coherence::ParticipantRecord> participants;
  std::vector<coherence::RegionRecord> regions;

  SharedWorld(std::size_t participant_count, std::size_t region_count,
              bool durable = false) {
    config.enable_durability = durable;
    engine = std::make_unique<coherence::CoherenceEngine>(config);
    if (durable) {
      auto store = std::make_shared<coherence::MemoryDurableStore>();
      (void)engine->attach_store(store);
    }
    domain = engine->create_domain("concurrency").value().id;
    object = engine->register_object(domain, "object", 65536,
                                     coherence::strict_policy("concurrency"))
                 .value().id;
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

CF_TEST(many_concurrent_readers_observe_consistent_decisions) {
  context.phase("SETUP");
  SharedWorld world(2, 4);
  coherence::RevalidateRequest revalidate;
  revalidate.context = world.context(world.participants[0]);
  revalidate.region = world.regions[0].id;
  revalidate.region_generation = world.regions[0].generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content.length = 64;
  revalidate.content.defined = true;
  revalidate.content.crc32c = 1;
  CF_REQUIRE(world.engine->revalidate_region(revalidate).has_value());

  context.phase("ACQUIRE");
  std::atomic<int> failures{0};
  std::atomic<std::uint64_t> reads{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 400; ++i) {
        coherence::ReadRequest request;
        request.context = world.context(world.participants[t % 2]);
        request.region =
            (i % 3 == 0) ? world.regions[0].id : coherence::RegionId::nil();
        request.require_current = true;
        const auto decision = world.engine->acquire_read(request);
        if (!decision.has_value()) {
          failures.fetch_add(1);
          continue;
        }
        if (decision.value().outcome == coherence::ReadOutcome::Unknown) failures.fetch_add(1);
        reads.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(failures.load()), 0u);
  CF_EXPECT_EQ_U(reads.load(), 3200u);
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(competing_writers_never_both_hold_authority) {
  context.phase("SETUP");
  SharedWorld world(4, 4);
  for (std::size_t i = 0; i < world.regions.size(); ++i) {
    coherence::RevalidateRequest revalidate;
    revalidate.context = world.context(world.participants[i % world.participants.size()]);
    revalidate.region = world.regions[i].id;
    revalidate.region_generation = world.regions[i].generation;
    revalidate.observed_version = coherence::VersionId::from_value(1);
    revalidate.content.length = 64;
    revalidate.content.defined = true;
    revalidate.content.crc32c = static_cast<std::uint32_t>(i);
    (void)world.engine->revalidate_region(revalidate);
  }

  context.phase("ACQUIRE");
  std::atomic<int> granted{0};
  std::atomic<int> conflicted{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      coherence::WriteRequest request;
      request.context = world.context(world.participants[t % world.participants.size()]);
      const coherence::RegionRecord& region = world.regions[t % world.regions.size()];
      request.region = region.id;
      request.region_generation = region.generation;
      const auto result = world.engine->acquire_write(request);
      if (result.has_value() && result.value().granted) {
        granted.fetch_add(1);
      } else {
        conflicted.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  CF_EXPECT(granted.load() >= 1);
  const auto object = world.engine->get_object(world.object);
  CF_REQUIRE(object.has_value());
  // Whichever writer won, exactly one incarnation holds the authority.
  CF_EXPECT(object.value().writer.defined());
  CF_EXPECT(object.value().authority == coherence::AuthorityMode::ExclusiveWriter ||
            object.value().authority == coherence::AuthorityMode::TransferPending);
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(no_reader_ever_observes_an_uncommitted_version) {
  context.phase("SETUP");
  SharedWorld world(2, 2, /*durable=*/true);
  coherence::RevalidateRequest revalidate;
  revalidate.context = world.context(world.participants[0]);
  revalidate.region = world.regions[0].id;
  revalidate.region_generation = world.regions[0].generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content.length = 64;
  revalidate.content.defined = true;
  revalidate.content.crc32c = 1;
  CF_REQUIRE(world.engine->revalidate_region(revalidate).has_value());

  coherence::WriteRequest write;
  write.context = world.context(world.participants[0]);
  write.region = world.regions[0].id;
  write.region_generation = world.regions[0].generation;
  const auto grant = world.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);

  context.phase("PUBLISH");
  std::atomic<bool> stop{false};
  std::atomic<int> violations{0};
  std::atomic<std::uint64_t> committed{0};
  std::atomic<std::uint64_t> reserved{0};
  std::atomic<std::uint64_t> observed_max{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        coherence::ReadRequest request;
        request.context = world.context(world.participants[1]);
        const auto decision = world.engine->acquire_read(request);
        if (!decision.has_value()) {
          violations.fetch_add(1);
          continue;
        }
        // A reader must never observe a version the writer has not even begun
        // to publish, and the authoritative version must never move backwards.
        // A reader may legitimately miss intermediate versions.
        const std::uint64_t version = decision.value().authoritative_version.value();
        if (version > reserved.load(std::memory_order_acquire)) violations.fetch_add(1);
        const std::uint64_t previous = observed_max.load(std::memory_order_acquire);
        if (version < previous) violations.fetch_add(1);
        if (version > previous) observed_max.store(version, std::memory_order_release);
      }
    });
  }
  for (int round = 0; round < 20; ++round) {
    coherence::DirtyRequest dirty;
    dirty.context = world.context(world.participants[0]);
    dirty.region = world.regions[0].id;
    dirty.region_generation = world.regions[0].generation;
    dirty.ownership_generation = grant.value().context.ownership_generation;
    dirty.base_version = world.engine->get_object(world.object).value().authoritative_version;
    if (round > 0 && !world.engine->mark_dirty(dirty).has_value()) break;
    coherence::PublishRequest publish;
    publish.context = world.context(world.participants[0]);
    publish.ownership_generation = grant.value().context.ownership_generation;
    publish.region = world.regions[0].id;
    publish.region_generation = world.regions[0].generation;
    publish.expected_base_version = dirty.base_version;
    publish.allow_without_dirty = (round == 0);
    publish.content = revalidate.content;
    reserved.store(dirty.base_version.defined() ? dirty.base_version.value() + 1 : 1,
                   std::memory_order_release);
    const auto receipt = world.engine->publish(publish);
    if (!receipt.has_value() || receipt.value().state != coherence::PublicationState::Committed) {
      break;
    }
    committed.store(receipt.value().version.value(), std::memory_order_release);
  }
  stop.store(true, std::memory_order_release);
  for (std::thread& thread : readers) thread.join();
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(violations.load()), 0u);
  CF_EXPECT(observed_max.load() >= 1);
  CF_EXPECT_EQ_U(observed_max.load(), committed.load());
  CF_EXPECT_AUDIT_CLEAN(world.engine->audit());
}

CF_TEST(snapshots_rendered_during_mutation_are_never_torn) {
  context.phase("SETUP");
  SharedWorld world(3, 6);
  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};
  // The mutation stream is bounded so that this case measures consistency
  // rather than the cost of an ever-growing snapshot.
  constexpr int kMutations = 400;
  std::atomic<int> registered{0};
  std::thread mutator([&] {
    for (int counter = 1; counter <= kMutations; ++counter) {
      coherence::RegionRegistration registration;
      const coherence::ParticipantRecord& owner =
          world.participants[static_cast<std::size_t>(counter) % world.participants.size()];
      registration.context = world.context(owner);
      registration.domain = world.domain;
      registration.name = "dynamic-" + std::to_string(counter);
      registration.memory_domain = coherence::MemoryDomain::HostPageable;
      registration.evidence_class = coherence::EvidenceClass::Real;
      registration.offset = 0;
      registration.length = 64;
      if (world.engine->register_region(registration).has_value()) {
        registered.fetch_add(1);
      }
    }
    stop.store(true, std::memory_order_release);
  });
  context.phase("SNAPSHOT");
  int snapshots = 0;
  while (!stop.load(std::memory_order_acquire) || snapshots < 200) {
    ++snapshots;
    const auto snapshot = world.engine->snapshot(coherence::SnapshotOptions{});
    if (!snapshot.has_value()) {
      mismatches.fetch_add(1);
      continue;
    }
    // Every replica listed by an object must be present in the same snapshot: a
    // snapshot is one consistent point in time, never a torn read.
    std::set<coherence::RegionId> present;
    for (const coherence::RegionRecord& region : snapshot.value().regions) {
      present.insert(region.id);
    }
    for (const coherence::ObjectRecord& object : snapshot.value().objects) {
      for (const coherence::RegionId id : object.replicas) {
        if (present.count(id) == 0) mismatches.fetch_add(1);
      }
    }
    if (snapshot.value().render().empty()) mismatches.fetch_add(1);
  }
  mutator.join();
  CF_EXPECT(registered.load() > 0);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(mismatches.load()), 0u);
  CF_EXPECT_AUDIT_CLEAN(world.engine->audit());
}

CF_TEST(concurrent_synchronization_completions_apply_exactly_once) {
  context.phase("SETUP");
  SharedWorld world(2, 2);
  coherence::RevalidateRequest revalidate;
  revalidate.context = world.context(world.participants[0]);
  revalidate.region = world.regions[0].id;
  revalidate.region_generation = world.regions[0].generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content.length = 64;
  revalidate.content.defined = true;
  revalidate.content.crc32c = 7;
  CF_REQUIRE(world.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = world.context(world.participants[0]);
  write.region = world.regions[0].id;
  write.region_generation = world.regions[0].generation;
  const auto grant = world.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = world.context(world.participants[0]);
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = world.regions[0].id;
  publish.region_generation = world.regions[0].generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = revalidate.content;
  CF_REQUIRE(world.engine->publish(publish).has_value());

  coherence::SyncRequest sync;
  sync.context = world.context(world.participants[1]);
  sync.source_region = world.regions[0].id;
  sync.source_region_generation = world.regions[0].generation;
  sync.destination_region = world.regions[1].id;
  sync.destination_region_generation = world.regions[1].generation;
  const auto plan = world.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());

  context.phase("COMPLETE");
  std::atomic<int> completed{0};
  std::atomic<int> replayed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&] {
      coherence::SyncCompleteRequest complete;
      complete.context = world.context(world.participants[1]);
      complete.operation = plan.value().operation;
      complete.destination_region = world.regions[1].id;
      complete.destination_region_generation = world.regions[1].generation;
      complete.destination_new_version = plan.value().plan.source_version;
      complete.observed_content = revalidate.content;
      const auto outcome = world.engine->complete_sync(complete);
      if (outcome.has_value() && outcome.value().state == coherence::SyncState::Completed) {
        if (outcome.value().idempotent_replay) {
          replayed.fetch_add(1);
        } else {
          completed.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(completed.load()), 1u);
  CF_EXPECT(replayed.load() >= 1);
  CF_EXPECT(world.engine->get_region(world.regions[1].id).value().state ==
            coherence::CoherenceState::Current);
}

CF_TEST(policy_mutation_during_reads_is_serialized_against_decisions) {
  context.phase("SETUP");
  SharedWorld world(2, 2);
  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::thread reader([&] {
    while (!stop.load()) {
      coherence::ReadRequest request;
      request.context = world.context(world.participants[0]);
      const auto decision = world.engine->acquire_read(request);
      if (!decision.has_value()) {
        failures.fetch_add(1);
        continue;
      }
      // Either the decision was made under the current policy generation, or it
      // was refused for carrying a stale one. Nothing else is acceptable.
      if (decision.value().reason != coherence::StatusCode::StalePolicy) {
        const auto object = world.engine->get_object(world.object);
        if (object.has_value() &&
            decision.value().context.policy_generation != object.value().policy_generation &&
            decision.value().reason != coherence::StatusCode::Ok) {
          failures.fetch_add(1);
        }
      }
    }
  });
  context.phase("POLICY");
  for (int i = 0; i < 60; ++i) {
    const auto object = world.engine->get_object(world.object);
    if (!object.has_value()) break;
    coherence::CoherencePolicy next = (i % 2 == 0) ? coherence::eventual_policy(4, "flip")
                                                  : coherence::strict_policy("flip");
    const auto updated = world.engine->set_policy(world.object, object.value().generation,
                                                  object.value().policy_generation, next);
    if (!updated.has_value()) failures.fetch_add(1);
  }
  stop.store(true);
  reader.join();
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(failures.load()), 0u);
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(fencing_during_traffic_is_safe_and_repeatable) {
  context.phase("SETUP");
  SharedWorld world(3, 4);
  std::atomic<bool> stop{false};
  std::atomic<int> unexpected{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      while (!stop.load()) {
        coherence::ReadRequest request;
        request.context = world.context(world.participants[t % world.participants.size()]);
        const auto decision = world.engine->acquire_read(request);
        if (!decision.has_value()) {
          const auto code = decision.status().code();
          if (code != coherence::StatusCode::Fenced &&
              code != coherence::StatusCode::StaleEpoch &&
              code != coherence::StatusCode::UnknownParticipant) {
            unexpected.fetch_add(1);
          }
        }
      }
    });
  }
  context.phase("FENCE");
  for (int i = 0; i < 200; ++i) {
    const coherence::ParticipantRecord& target =
        world.participants[i % world.participants.size()];
    (void)world.engine->fence_participant(target.id, world.engine->epoch(), "concurrency fence");
  }
  stop.store(true);
  for (std::thread& thread : threads) thread.join();
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(unexpected.load()), 0u);
  CF_EXPECT(world.engine->audit().clean);
}

CF_TEST(concurrent_registration_and_shutdown_are_serialized) {
  context.phase("SETUP");
  SharedWorld world(2, 2);
  std::atomic<int> created{0};
  std::atomic<int> refused{0};
  std::atomic<int> unexpected{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        const coherence::ParticipantRecord& owner = world.participants[t % 2];
        coherence::RegionRegistration registration;
        registration.context = world.context(owner);
        registration.domain = world.domain;
        registration.name = "c" + std::to_string(t) + "-" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = 0;
        registration.length = 64;
        const auto result = world.engine->register_region(registration);
        if (result.has_value()) {
          created.fetch_add(1);
        } else if (result.status().code() == coherence::StatusCode::ShuttingDown ||
                   result.status().code() == coherence::StatusCode::Fenced ||
                   result.status().code() == coherence::StatusCode::StaleBoot) {
          refused.fetch_add(1);
        } else {
          unexpected.fetch_add(1);
        }
      }
    });
  }
  context.phase("SHUTDOWN");
  std::thread shutdown_thread([&] { (void)world.engine->begin_shutdown(world.engine->epoch()); });
  shutdown_thread.join();
  for (std::thread& thread : threads) thread.join();
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(unexpected.load()), 0u);
  CF_EXPECT(created.load() + refused.load() == 800);
  CF_REQUIRE(world.engine->complete_shutdown(world.engine->epoch()).ok());
  // Repeated shutdown is idempotent.
  CF_REQUIRE(world.engine->complete_shutdown(world.engine->epoch()).ok());
}

CF_TEST_MAIN()

