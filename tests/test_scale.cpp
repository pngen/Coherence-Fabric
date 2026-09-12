// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Large-scale durable state: the protocol and persistence limits must not
// accidentally cap the scale of the runtime.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "test_framework.hpp"

namespace {

std::filesystem::path scale_directory() {
  const std::filesystem::path path =
      std::filesystem::current_path() / "scale-scratch" / "store";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  return path;
}

} // namespace

CF_TEST(ten_thousand_objects_and_replicas_survive_save_load_and_audit) {
  context.phase("SETUP");
  constexpr std::uint64_t kObjects = 10000;
  const std::filesystem::path directory = scale_directory();
  coherence::EngineConfig config;
  config.max_objects = kObjects + 16;
  config.max_regions = kObjects * 2 + 16;
  config.max_object_length = 1ull << 32;

  coherence::CoherencePolicy policy = coherence::strict_policy("scale");

  const std::chrono::steady_clock::time_point create_start = std::chrono::steady_clock::now();
  {
    coherence::CoherenceEngine engine(config);
    // A bulk load writes tens of thousands of records. Every record is still
    // written before the state it describes becomes observable; the durability
    // barrier is issued once explicitly rather than tens of thousands of times.
    coherence::StoreLimits limits;
    limits.barrier_per_append = false;
    auto store = std::make_shared<coherence::FileDurableStore>(directory, limits);
    const auto attached = engine.attach_store(store);
    CF_REQUIRE(attached.has_value());
    const auto domain = engine.create_domain("scale");
    CF_REQUIRE(domain.has_value());
    const auto participant = engine.register_participant(
        "scale", coherence::generate_participant_boot_id(), engine.epoch(), "scale-node");
    CF_REQUIRE(participant.has_value());

    context.phase("CREATE");
    std::vector<coherence::ObjectId> objects;
    objects.reserve(kObjects);
    for (std::uint64_t i = 0; i < kObjects; ++i) {
      auto object = engine.register_object(domain.value().id, "object-" + std::to_string(i), 4096,
                                           policy);
      if (!object.has_value()) {
        CF_EXPECT(false);
        break;
      }
      objects.push_back(object.value().id);
      coherence::RegionRegistration registration;
      registration.context.epoch = engine.epoch();
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
      if (!engine.register_region(registration).has_value()) {
        CF_EXPECT(false);
        break;
      }
    }
    CF_EXPECT_EQ_U(static_cast<std::uint64_t>(objects.size()), kObjects);
    context.mark("objects=" + std::to_string(objects.size()) + " create_ms=" +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - create_start)
                                    .count()));

    context.phase("AUDIT");
    const coherence::AuditReport report = engine.audit();
    CF_EXPECT(report.clean);
    CF_EXPECT_EQ_U(report.objects_checked, kObjects);
    CF_EXPECT_EQ_U(report.regions_checked, kObjects);

    context.phase("SAVE");
    CF_REQUIRE(engine.flush_durable().ok());
    CF_REQUIRE(engine.begin_shutdown(engine.epoch()).ok());
    CF_REQUIRE(engine.complete_shutdown(engine.epoch()).ok());
  }

  context.phase("LOAD");
  const std::chrono::steady_clock::time_point load_start = std::chrono::steady_clock::now();
  coherence::CoherenceEngine reloaded(config);
  auto store = std::make_shared<coherence::FileDurableStore>(directory);
  const auto loaded = reloaded.attach_store(store);
  CF_REQUIRE(loaded.has_value());
  context.mark("load_ms=" +
               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - load_start)
                                  .count()));
  CF_EXPECT_EQ_U(loaded.value().objects_restored, kObjects);
  CF_EXPECT_EQ_U(loaded.value().regions_restored, kObjects);
  CF_EXPECT_EQ_U(loaded.value().regions_downgraded, kObjects);
  CF_EXPECT_EQ_U(loaded.value().corrupt_records_skipped, 0u);

  context.phase("AUDIT_RELOADED");
  const coherence::AuditReport report = reloaded.audit();
  CF_EXPECT(report.clean);
  CF_EXPECT_EQ_U(report.objects_checked, kObjects);
  CF_EXPECT_EQ_U(report.regions_checked, kObjects);

  context.phase("VERIFY");
  const auto snapshot = reloaded.snapshot(coherence::SnapshotOptions{});
  CF_REQUIRE(snapshot.has_value());
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(snapshot.value().objects.size()), kObjects);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(snapshot.value().regions.size()), kObjects);
  for (const coherence::RegionRecord& region : snapshot.value().regions) {
    CF_REQUIRE(region.state == coherence::CoherenceState::RevalidationRequired);
  }
  for (const coherence::ObjectRecord& object : snapshot.value().objects) {
    CF_REQUIRE(!object.writer.defined());
    CF_REQUIRE(object.reads.empty());
  }
  CF_REQUIRE(reloaded.begin_shutdown(reloaded.epoch()).ok());
}

CF_TEST_MAIN()

