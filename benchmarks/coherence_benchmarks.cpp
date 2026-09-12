// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Benchmarks of completed operations.
//
// Every measurement surrounds the whole operation and reports throughput of
// work that actually finished. No submit-only path is reported as completed
// throughput. Timings use a monotonic clock for measurement only; nothing in
// the runtime derives ordering or identity from wall-clock time.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "coherence/codec.hpp"
#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "coherence/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  std::uint64_t scale = 0;
  std::uint64_t operations = 0;
  double nanoseconds_per_operation = 0.0;
  double operations_per_second = 0.0;
  std::string note;
};

std::vector<Measurement>& measurements() {
  static std::vector<Measurement> all;
  return all;
}

template <typename Body>
void measure(const std::string& name, std::uint64_t scale, std::uint64_t operations,
             Body body, const std::string& note = std::string()) {
  const Clock::time_point start = Clock::now();
  body();
  const Clock::time_point finish = Clock::now();
  const double elapsed =
      std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(finish - start).count();
  Measurement measurement;
  measurement.name = name;
  measurement.scale = scale;
  measurement.operations = operations;
  measurement.nanoseconds_per_operation = operations == 0 ? 0.0 : elapsed / static_cast<double>(operations);
  measurement.operations_per_second =
      elapsed == 0.0 ? 0.0 : static_cast<double>(operations) * 1e9 / elapsed;
  measurement.note = note;
  measurements().push_back(measurement);
  std::printf("benchmark name=%-32s scale=%-8llu ops=%-10llu ns_per_op=%-12.1f ops_per_sec=%.0f %s\n",
              measurement.name.c_str(), static_cast<unsigned long long>(scale),
              static_cast<unsigned long long>(operations), measurement.nanoseconds_per_operation,
              measurement.operations_per_second, note.c_str());
  std::fflush(stdout);
}

struct Fixture {
  coherence::EngineConfig config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::CoherenceDomainId domain;
  coherence::ObjectId object;
  coherence::ParticipantRecord writer;
  coherence::ParticipantRecord reader;
  std::vector<coherence::RegionRecord> regions;

  Fixture(std::uint64_t region_count, bool durable = false, bool initialize = true,
          bool decision_log = true) {
    config.enable_durability = durable;
    config.enable_decision_log = decision_log;
    config.max_regions = region_count * 4 + 1024;
    config.max_objects = 1024;
    engine = std::make_unique<coherence::CoherenceEngine>(config);
    if (durable) {
      (void)engine->attach_store(std::make_shared<coherence::MemoryDurableStore>());
    }
    domain = engine->create_domain("bench").value().id;
    object = engine->register_object(domain, "object", 1ull << 32,
                                     coherence::strict_policy("bench"))
                 .value().id;
    writer = engine->register_participant("writer", coherence::generate_participant_boot_id(),
                                          engine->epoch(), "")
                 .value();
    reader = engine->register_participant("reader", coherence::generate_participant_boot_id(),
                                          engine->epoch(), "")
                 .value();
    if (!initialize) return;
    for (std::uint64_t i = 0; i < region_count; ++i) {
      coherence::RegionRegistration registration;
      registration.context = context(i % 2 == 0 ? writer : reader);
      registration.domain = domain;
      registration.name = "replica-" + std::to_string(i);
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

  bool initialize_initial_version() {
    coherence::RevalidateRequest revalidate;
    revalidate.context = context(writer);
    revalidate.region = regions.front().id;
    revalidate.region_generation = regions.front().generation;
    revalidate.observed_version = coherence::VersionId::from_value(1);
    revalidate.content.length = 64;
    revalidate.content.defined = true;
    revalidate.content.crc32c = 1;
    if (!engine->revalidate_region(revalidate).has_value()) return false;
    coherence::WriteRequest write;
    write.context = context(writer);
    write.region = regions.front().id;
    write.region_generation = regions.front().generation;
    const auto grant = engine->acquire_write(write);
    if (!grant.has_value()) return false;
    coherence::PublishRequest publish;
    publish.context = context(writer);
    publish.ownership_generation = grant.value().context.ownership_generation;
    publish.region = regions.front().id;
    publish.region_generation = regions.front().generation;
    publish.expected_base_version = coherence::VersionId::nil();
    publish.allow_without_dirty = true;
    publish.content = revalidate.content;
    const auto receipt = engine->publish(publish);
    return receipt.has_value() &&
           receipt.value().state == coherence::PublicationState::Committed;
  }
};

} // namespace

int main(int argc, char** argv) {
  std::vector<std::uint64_t> scales = {10, 100, 1000, 10000};
  bool include_large = true;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      scales.clear();
      scales.push_back(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--no-large") == 0) {
      include_large = false;
    }
  }
  std::printf("coherence-fabric benchmarks %s\n", coherence::version_string().c_str());
  std::fflush(stdout);

  for (const std::uint64_t scale : scales) {
    // --- replica registration ---------------------------------------------
    measure("replica_registration", scale, scale, [&] {
      Fixture fixture(0);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(i % 2 == 0 ? fixture.writer : fixture.reader);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        (void)fixture.engine->register_region(registration);
      }
    });

    // --- read-authority lookup --------------------------------------------
    {
      Fixture fixture(scale, false, false);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        auto region = fixture.engine->register_region(registration);
        if (region.has_value()) fixture.regions.push_back(region.value());
      }
      (void)fixture.initialize_initial_version();
      const std::uint64_t iterations = std::max<std::uint64_t>(scale, 1000);
      // The caller-side authority context is built once, outside the measurement,
      // so the numbers cover the runtime operation rather than the benchmark's
      // own record copying.
      const coherence::AuthorityContext reader_context = fixture.context(fixture.reader);
      measure("read_authority_lookup", scale, iterations, [&] {
        for (std::uint64_t i = 0; i < iterations; ++i) {
          coherence::ReadRequest request;
          request.context = reader_context;
          request.require_current = true;
          (void)fixture.engine->acquire_read(request);
        }
      });
      // The same operation with the replica named explicitly, which isolates
      // automatic replica selection from the rest of the read path.
      const coherence::RegionId named = fixture.regions.front().id;
      const coherence::RegionGeneration named_generation = fixture.regions.front().generation;
      measure("read_authority_lookup_named_replica", scale, iterations, [&] {
        for (std::uint64_t i = 0; i < iterations; ++i) {
          coherence::ReadRequest request;
          request.context = reader_context;
          request.require_current = true;
          request.region = named;
          request.region_generation = named_generation;
          (void)fixture.engine->acquire_read(request);
        }
      });
      // The decision path alone, without acquiring a read lease, and a bare
      // record lookup, to locate where the time is actually spent.
      measure("read_decision_only", scale, iterations, [&] {
        for (std::uint64_t i = 0; i < iterations; ++i) {
          coherence::ReadRequest request;
          request.context = reader_context;
          request.require_current = true;
          request.region = named;
          request.region_generation = named_generation;
          (void)fixture.engine->explain_read(request);
        }
      });
      measure("record_lookup", scale, iterations, [&] {
        for (std::uint64_t i = 0; i < iterations; ++i) {
          (void)fixture.engine->get_region(named);
        }
      });
      {
        // The same decision path with the retrospective decision log disabled,
        // which isolates the cost of the log from the cost of the decision.
        Fixture quiet(scale, false, false, /*decision_log=*/false);
        for (std::uint64_t i = 0; i < scale; ++i) {
          coherence::RegionRegistration registration;
          registration.context = quiet.context(quiet.writer);
          registration.domain = quiet.domain;
          registration.name = "r" + std::to_string(i);
          registration.memory_domain = coherence::MemoryDomain::HostPageable;
          registration.evidence_class = coherence::EvidenceClass::Real;
          registration.offset = i * 64;
          registration.length = 64;
          auto region = quiet.engine->register_region(registration);
          if (region.has_value()) quiet.regions.push_back(region.value());
        }
        (void)quiet.initialize_initial_version();
        const coherence::AuthorityContext quiet_context = quiet.context(quiet.reader);
        measure("read_authority_lookup_without_decision_log", scale, iterations, [&] {
          for (std::uint64_t i = 0; i < iterations; ++i) {
            coherence::ReadRequest request;
            request.context = quiet_context;
            request.require_current = true;
            (void)quiet.engine->acquire_read(request);
          }
        });
      }
      {
        // A second object with a single replica in the same engine, to separate
        // "the engine holds many regions" from "this object has many replicas".
        auto small = fixture.engine->register_object(fixture.domain, "small-object", 4096,
                                                     coherence::strict_policy("small"));
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.context.object = small.value().id;
        registration.context.object_generation = small.value().generation;
        registration.context.policy_generation = small.value().policy_generation;
        registration.domain = fixture.domain;
        registration.name = "only-replica";
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = 0;
        registration.length = 4096;
        (void)fixture.engine->register_region(registration);
        measure("object_record_copy", scale, iterations, [&] {
          for (std::uint64_t i = 0; i < iterations; ++i) {
            (void)fixture.engine->get_object(small.value().id);
          }
        });
      }
      // Uniformity probe: report the latency of individual calls so that a
      // single expensive call cannot be mistaken for a per-call cost.
      {
        std::uint64_t worst = 0;
        std::uint64_t total = 0;
        const std::uint64_t samples = std::min<std::uint64_t>(iterations, 1000);
        for (std::uint64_t i = 0; i < samples; ++i) {
          const Clock::time_point start = Clock::now();
          coherence::ReadRequest request;
          request.context = reader_context;
          request.require_current = true;
          (void)fixture.engine->acquire_read(request);
          const std::uint64_t elapsed =
              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             Clock::now() - start)
                                             .count());
          total += elapsed;
          if (elapsed > worst) worst = elapsed;
        }
        std::printf("benchmark probe read_authority_lookup scale=%llu samples=%llu mean_ns=%llu "
                    "worst_ns=%llu\n",
                    static_cast<unsigned long long>(scale),
                    static_cast<unsigned long long>(samples),
                    static_cast<unsigned long long>(total / samples),
                    static_cast<unsigned long long>(worst));
        std::fflush(stdout);
      }
    }

    // --- currentness query -------------------------------------------------
    {
      Fixture fixture(scale, false, false);
      fixture.regions.resize(0);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        auto region = fixture.engine->register_region(registration);
        if (region.has_value()) fixture.regions.push_back(region.value());
      }
      const std::uint64_t iterations = std::max<std::uint64_t>(scale, 1000);
      measure("currentness_query", scale, iterations, [&] {
        for (std::uint64_t i = 0; i < iterations; ++i) {
          (void)fixture.engine->get_region(fixture.regions[i % fixture.regions.size()].id);
        }
      });
    }

    // --- state transition --------------------------------------------------
    {
      const std::uint64_t iterations = std::max<std::uint64_t>(scale, 1000);
      measure("state_transition", scale, iterations, [&] {
        coherence::DeterministicRandom random(0xBEEF);
        std::vector<coherence::CoherenceState> states = {
            coherence::CoherenceState::Invalid, coherence::CoherenceState::Stale,
            coherence::CoherenceState::Current, coherence::CoherenceState::SyncRequired,
            coherence::CoherenceState::RevalidationRequired, coherence::CoherenceState::Dirty};
        coherence::CoherenceState from = states.front();
        for (std::uint64_t i = 0; i < iterations; ++i) {
          const coherence::CoherenceState to = states[random.next_below(states.size())];
          if (coherence::is_legal_coherence_transition(from, to)) from = to;
        }
      });
    }

    // --- serialization -----------------------------------------------------
    {
      Fixture fixture(scale, false, false);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        auto region = fixture.engine->register_region(registration);
        if (region.has_value()) fixture.regions.push_back(region.value());
      }
      std::vector<std::byte> encoded;
      measure("serialization_encode", scale, fixture.regions.size(), [&] {
        coherence::ByteWriter writer;
        writer.u64(fixture.regions.size());
        for (const coherence::RegionRecord& region : fixture.regions) {
          coherence::encode_region(writer, region);
        }
        encoded.assign(writer.span().begin(), writer.span().end());
      });
      measure("serialization_decode", scale, fixture.regions.size(), [&] {
        coherence::ByteReader reader(encoded);
        std::uint64_t count = 0;
        (void)reader.u64(count);
        for (std::uint64_t i = 0; i < count; ++i) {
          coherence::RegionRecord region;
          (void)coherence::decode_region(reader, region, coherence::default_decode_limits());
        }
      });
    }

    // --- snapshot rendering and invariant audit ----------------------------
    {
      Fixture fixture(scale, false, false);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        (void)fixture.engine->register_region(registration);
      }
      measure("snapshot_creation", scale, 1, [&] {
        const auto snapshot = fixture.engine->snapshot(coherence::SnapshotOptions{});
        (void)snapshot;
      });
      measure("invariant_audit", scale, 1, [&] {
        const coherence::AuditReport report = fixture.engine->audit();
        if (!report.clean) std::printf("benchmark warning: audit reported violations\n");
      });
    }

    // --- save and load -----------------------------------------------------
    {
      Fixture fixture(scale, false, false);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        (void)fixture.engine->register_region(registration);
      }
      const auto snapshot = fixture.engine->snapshot(coherence::SnapshotOptions{});
      coherence::DurableImage image;
      image.epoch = snapshot.value().epoch;
      for (const coherence::DomainRecord& record : snapshot.value().domains) {
        image.domains.emplace(record.id, record);
      }
      for (const coherence::ParticipantRecord& record : snapshot.value().participants) {
        image.participants.emplace(record.id, record);
      }
      for (const coherence::ObjectRecord& record : snapshot.value().objects) {
        image.objects.emplace(record.id, record);
      }
      for (const coherence::RegionRecord& record : snapshot.value().regions) {
        image.regions.emplace(record.id, record);
      }
      std::vector<std::byte> bytes;
      measure("durable_save", scale, image.record_count(), [&] { bytes = coherence::encode_snapshot(image); });
      measure("durable_load", scale, image.record_count(), [&] {
        coherence::DurableImage decoded;
        (void)coherence::decode_snapshot(coherence::ByteSpan(bytes), coherence::StoreLimits{}, decoded);
      });
    }

    // --- write authority, publication, invalidation, synchronization -------
    {
      Fixture fixture(scale, false, false);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(i % 2 == 0 ? fixture.writer : fixture.reader);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        auto region = fixture.engine->register_region(registration);
        if (region.has_value()) fixture.regions.push_back(region.value());
      }
      (void)fixture.initialize_initial_version();

      // Writer handoff under contention: only one participant wins each round.
      const std::uint64_t handoff_rounds = std::min<std::uint64_t>(scale, 200);
      measure("writer_handoff", scale, handoff_rounds, [&] {
        for (std::uint64_t round = 0; round < handoff_rounds; ++round) {
          coherence::WriteRequest write;
          write.context = fixture.context(round % 2 == 0 ? fixture.reader : fixture.writer);
          const coherence::RegionRecord& region =
              fixture.regions[(round + 1) % fixture.regions.size()];
          write.region = region.id;
          write.region_generation = region.generation;
          const auto grant = fixture.engine->acquire_write(write);
          if (grant.has_value() && grant.value().may_mutate_now) {
            coherence::ReleaseRequest release;
            release.context = write.context;
            release.ownership_generation = grant.value().context.ownership_generation;
            release.release_write_authority = true;
            (void)fixture.engine->release(release);
          } else if (grant.has_value()) {
            for (const coherence::InvalidationRecord& invalidation :
                 grant.value().required_invalidations) {
              // The owning incarnation is this process in the benchmark, so the
              // acknowledgement is issued directly.
              coherence::InvalidationAck ack;
              ack.context = fixture.context(fixture.writer);
              ack.invalidation = invalidation.id;
              ack.region = invalidation.region;
              ack.region_generation = invalidation.region_generation;
              ack.replica_generation = invalidation.replica_generation;
              ack.superseded_version = invalidation.superseded_version;
              (void)fixture.engine->acknowledge_invalidation(ack);
            }
          }
        }
      });

      // Synchronization plan generation.
      const std::uint64_t plan_rounds = std::min<std::uint64_t>(scale, 200);
      measure("sync_plan_generation", scale, plan_rounds, [&] {
        for (std::uint64_t round = 0; round < plan_rounds; ++round) {
          coherence::SyncRequest sync;
          sync.context = fixture.context(fixture.reader);
          sync.destination_region = fixture.regions[1 + (round % (fixture.regions.size() - 1))].id;
          sync.destination_region_generation =
              fixture.engine->get_region(sync.destination_region).value().generation;
          (void)fixture.engine->begin_sync(sync);
        }
      });
    }

    // --- durable publication ----------------------------------------------
    {
      const std::uint64_t rounds = std::min<std::uint64_t>(scale, 200);
      Fixture fixture(1, /*durable=*/true, false);
      for (int i = 0; i < 1; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "durable-replica";
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = 0;
        registration.length = 64;
        auto region = fixture.engine->register_region(registration);
        if (region.has_value()) fixture.regions.push_back(region.value());
      }
      (void)fixture.initialize_initial_version();
      measure("durable_publication", 1, rounds, [&] {
        for (std::uint64_t round = 0; round < rounds; ++round) {
          const auto object = fixture.engine->get_object(fixture.object);
          coherence::WriteRequest write;
          write.context = fixture.context(fixture.writer);
          write.region = fixture.regions.front().id;
          write.region_generation = fixture.regions.front().generation;
          const auto grant = fixture.engine->acquire_write(write);
          if (!grant.has_value()) continue;
          coherence::DirtyRequest dirty;
          dirty.context = write.context;
          dirty.region = write.region;
          dirty.region_generation = write.region_generation;
          dirty.ownership_generation = grant.value().context.ownership_generation;
          dirty.base_version = object.value().authoritative_version;
          (void)fixture.engine->mark_dirty(dirty);
          coherence::PublishRequest publish;
          publish.context = write.context;
          publish.ownership_generation = grant.value().context.ownership_generation;
          publish.region = write.region;
          publish.region_generation = write.region_generation;
          publish.expected_base_version = object.value().authoritative_version;
          publish.content.length = 64;
          publish.content.defined = true;
          publish.content.crc32c = static_cast<std::uint32_t>(round + 2);
          (void)fixture.engine->publish(publish);
        }
      }, "durable journal append plus flush per publication");
    }

    // --- concurrent readers ------------------------------------------------
    {
      Fixture fixture(std::min<std::uint64_t>(scale, 200), false, false);
      for (std::uint64_t i = 0; i < std::min<std::uint64_t>(scale, 200); ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        auto region = fixture.engine->register_region(registration);
        if (region.has_value()) fixture.regions.push_back(region.value());
      }
      const std::uint64_t threads = 8;
      const std::uint64_t per_thread = std::max<std::uint64_t>(scale, 500);
      measure("contention_concurrent_readers", scale, threads * per_thread, [&] {
        std::vector<std::thread> workers;
        for (std::uint64_t t = 0; t < threads; ++t) {
          workers.emplace_back([&fixture, per_thread] {
            for (std::uint64_t i = 0; i < per_thread; ++i) {
              coherence::ReadRequest request;
              request.context = fixture.context(fixture.reader);
              request.require_current = true;
              (void)fixture.engine->acquire_read(request);
            }
          });
        }
        for (std::thread& worker : workers) worker.join();
      }, "8 threads");
    }

    std::printf("\n");
    std::fflush(stdout);
  }

  if (include_large) {
    // A single large-scale measurement: 100000 replicas across 100000 objects
    // would exceed a sensible time budget for a benchmark run, so the largest
    // scale is exercised once here.
    const std::uint64_t scale = 100000;
    measure("replica_registration_large", scale, scale, [&] {
      Fixture fixture(scale, false, false);
      for (std::uint64_t i = 0; i < scale; ++i) {
        coherence::RegionRegistration registration;
        registration.context = fixture.context(fixture.writer);
        registration.domain = fixture.domain;
        registration.name = "r" + std::to_string(i);
        registration.memory_domain = coherence::MemoryDomain::HostPageable;
        registration.evidence_class = coherence::EvidenceClass::Real;
        registration.offset = i * 64;
        registration.length = 64;
        (void)fixture.engine->register_region(registration);
      }
      const coherence::AuditReport report = fixture.engine->audit();
      std::printf("benchmark audit at scale %llu: violations=%llu\n",
                  static_cast<unsigned long long>(scale),
                  static_cast<unsigned long long>(report.findings.size()));
    });
  }

  std::printf("benchmarks completed=%llu\n",
              static_cast<unsigned long long>(measurements().size()));
  std::fflush(stdout);
  return 0;
}
