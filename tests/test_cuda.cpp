// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Real CUDA proof.
//
// This exercises a real device allocation, a real host-to-device transfer, a
// real kernel mutation, a real device-to-host transfer and a real CPU/reference
// parity check, with the coherence metadata transitions driven by the actual
// bytes. It does NOT claim GPU hardware cache coherence: every transition below
// is enforced by software through explicit transfers and explicit authority.
#include <memory>
#include <string>
#include <vector>

#include "coherence/adapters/cuda_memory.hpp"
#include "coherence/adapters/region_store.hpp"
#include "coherence/engine.hpp"
#include "test_framework.hpp"

namespace {

constexpr std::uint64_t kExtent = 1u << 20;  // 1 MiB
constexpr std::uint64_t kSeed = 0x1234ABCDull;
constexpr std::uint32_t kDelta = 0x5Au;

} // namespace

CF_TEST(cuda_device_is_discovered_or_reported_unsupported) {
  context.phase("SETUP");
  const coherence::adapters::CudaCapability capability = coherence::adapters::probe_cuda();
  context.mark(capability.render());
  if (!capability.available) {
    context.mark("CUDA is UNSUPPORTED in this environment: " + capability.reason);
    CF_EXPECT(!capability.reason.empty());
    return;
  }
  CF_EXPECT(!capability.devices.empty());
  for (const coherence::adapters::CudaDeviceInfo& device : capability.devices) {
    CF_EXPECT(!device.name.empty());
    CF_EXPECT(device.total_memory > 0);
    CF_EXPECT(device.compute_major >= 1);
  }
}

CF_TEST(real_cuda_memory_transfers_and_kernel_mutation_match_the_cpu_reference) {
  context.phase("SETUP");
  const coherence::adapters::CudaCapability capability = coherence::adapters::probe_cuda();
  if (!capability.available) {
    context.mark("CUDA is UNSUPPORTED in this environment; the CUDA proof is not run");
    CF_EXPECT(true);
    return;
  }
  const int device = capability.devices.front().index;
  const auto baseline_before = coherence::adapters::cuda_memory_info(device);
  CF_REQUIRE(baseline_before.has_value());

  context.phase("ALLOCATE");
  auto buffer = coherence::adapters::CudaBuffer::allocate(kExtent, device);
  CF_REQUIRE(buffer.has_value());
  CF_EXPECT_EQ_U(buffer.value().bytes(), kExtent);

  context.phase("H2D");
  const std::vector<std::byte> host_source = coherence::adapters::make_pattern(kExtent, kSeed);
  CF_REQUIRE(buffer.value().upload(coherence::ByteSpan(host_source)).ok());
  auto round_trip = buffer.value().download();
  CF_REQUIRE(round_trip.has_value());
  // The device really holds the bytes that were uploaded.
  CF_EXPECT(round_trip.value() == host_source);

  context.phase("KERNEL");
  CF_REQUIRE(buffer.value().mutate(kSeed, kDelta).ok());
  auto mutated_device = buffer.value().download();
  CF_REQUIRE(mutated_device.has_value());
  CF_EXPECT(mutated_device.value() != host_source);
  std::vector<std::byte> reference = host_source;
  coherence::adapters::reference_mutation(coherence::MutableByteSpan(reference), kSeed, kDelta);
  // The kernel and the CPU reference implement the same transform.
  CF_EXPECT(mutated_device.value() == reference);

  context.phase("D2H");
  const coherence::ContentFingerprint device_fingerprint =
      coherence::fingerprint_bytes(coherence::ByteSpan(mutated_device.value()));
  const coherence::ContentFingerprint reference_fingerprint =
      coherence::fingerprint_bytes(coherence::ByteSpan(reference));
  CF_EXPECT(device_fingerprint == reference_fingerprint);

  context.phase("CLEANUP");
  CF_REQUIRE(buffer.value().synchronize().ok());
  buffer.value() = coherence::adapters::CudaBuffer{};
  const auto baseline_after = coherence::adapters::cuda_memory_info(device);
  CF_REQUIRE(baseline_after.has_value());
  // The device baseline returns once the allocation is released.
  const std::uint64_t slack = 64ull * 1024 * 1024;
  CF_EXPECT(baseline_after.value().first + slack >= baseline_before.value().first);
}

CF_TEST(coherence_metadata_follows_real_device_bytes) {
  context.phase("SETUP");
  const coherence::adapters::CudaCapability capability = coherence::adapters::probe_cuda();
  if (!capability.available) {
    context.mark("CUDA is UNSUPPORTED in this environment; the device coherence proof is not run");
    CF_EXPECT(true);
    return;
  }
  const int device = capability.devices.front().index;

  coherence::EngineConfig config;
  config.enable_durability = false;
  coherence::CoherenceEngine engine(config);
  coherence::adapters::RegionStore store;
  const auto domain = engine.create_domain("cuda");
  CF_REQUIRE(domain.has_value());
  const auto object = engine.register_object(domain.value().id, "tensor", kExtent,
                                            coherence::strict_policy("cuda-strict"));
  CF_REQUIRE(object.has_value());

  auto host_region = store.add_host_region("host-mirror", kExtent,
                                           coherence::MemoryDomain::HostPageable, kSeed);
  CF_REQUIRE(host_region.has_value());
  auto buffer = coherence::adapters::CudaBuffer::allocate(kExtent, device);
  CF_REQUIRE(buffer.has_value());
  auto device_region = store.attach_device_region(
      "device", "cuda", coherence::MemoryDomain::AcceleratorLocal,
      coherence::EvidenceClass::Real, kExtent, buffer.value().device_address());
  CF_REQUIRE(device_region.has_value());

  coherence::AuthorityContext authority;
  authority.epoch = engine.epoch();
  authority.object = object.value().id;
  authority.object_generation = object.value().generation;
  authority.policy_generation = object.value().policy_generation;

  auto register_region = [&](const std::string& name, coherence::RegionId id) {
    coherence::RegionRegistration registration;
    registration.context = authority;
    registration.domain = domain.value().id;
    registration.requested_id = id;
    registration.name = name;
    registration.memory_domain = store.at(id).memory_domain;
    registration.evidence_class = store.at(id).evidence_class;
    registration.offset = 0;
    registration.length = kExtent;
    registration.address_hint = store.at(id).address_hint;
    return engine.register_region(registration);
  };

  const auto alpha = engine.register_participant("host-agent",
                                                 coherence::generate_participant_boot_id(),
                                                 engine.epoch(), "host");
  CF_REQUIRE(alpha.has_value());
  authority.participant = alpha.value().id;
  authority.boot = alpha.value().boot;
  const auto host_record = register_region("host-mirror", host_region.value());
  CF_REQUIRE(host_record.has_value());

  context.phase("HOST_CURRENT");
  coherence::RevalidateRequest revalidate;
  revalidate.context = authority;
  revalidate.region = host_record.value().id;
  revalidate.region_generation = host_record.value().generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = store.fingerprint(host_region.value()).value();
  CF_REQUIRE(engine.revalidate_region(revalidate).has_value());
  CF_EXPECT(engine.get_region(host_record.value().id).value().state ==
            coherence::CoherenceState::Current);

  context.phase("PUBLISH_V1");
  coherence::WriteRequest write;
  write.context = authority;
  write.region = host_record.value().id;
  write.region_generation = host_record.value().generation;
  const auto grant = engine.acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().may_mutate_now);
  coherence::PublishRequest publish;
  publish.context = authority;
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = host_record.value().id;
  publish.region_generation = host_record.value().generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = store.fingerprint(host_region.value()).value();
  const auto first = engine.publish(publish);
  CF_REQUIRE(first.has_value());
  CF_REQUIRE(first.value().state == coherence::PublicationState::Committed);
  // The host incarnation hands its exclusive authority back before another
  // participant can take it.
  coherence::ReleaseRequest handback;
  handback.context = authority;
  handback.ownership_generation = grant.value().context.ownership_generation;
  handback.release_write_authority = true;
  CF_REQUIRE(engine.release(handback).has_value());

  context.phase("SYNC_TO_DEVICE");
  const auto device_participant = engine.register_participant(
      "device-agent", coherence::generate_participant_boot_id(), engine.epoch(), "device");
  CF_REQUIRE(device_participant.has_value());
  coherence::AuthorityContext device_context = authority;
  device_context.participant = device_participant.value().id;
  device_context.boot = device_participant.value().boot;
  coherence::RegionRegistration device_registration;
  device_registration.context = device_context;
  device_registration.domain = domain.value().id;
  device_registration.requested_id = device_region.value();
  device_registration.name = "device";
  device_registration.memory_domain = coherence::MemoryDomain::AcceleratorLocal;
  device_registration.evidence_class = coherence::EvidenceClass::Real;
  device_registration.offset = 0;
  device_registration.length = kExtent;
  const auto device_record = engine.register_region(device_registration);
  CF_REQUIRE(device_record.has_value());

  coherence::SyncRequest sync;
  sync.context = device_context;
  sync.source_region = host_record.value().id;
  sync.source_region_generation = host_record.value().generation;
  sync.destination_region = device_record.value().id;
  sync.destination_region_generation = device_record.value().generation;
  sync.transport = "cuda_memcpy";
  const auto plan = engine.begin_sync(sync);
  CF_REQUIRE(plan.has_value());
  CF_REQUIRE(buffer.value().upload(store.at(host_region.value()).bytes()).ok());
  auto echoed = buffer.value().download();
  CF_REQUIRE(echoed.has_value());
  coherence::SyncCompleteRequest complete;
  complete.context = device_context;
  complete.operation = plan.value().operation;
  complete.destination_region = device_record.value().id;
  complete.destination_region_generation = device_record.value().generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = coherence::fingerprint_bytes(coherence::ByteSpan(echoed.value()));
  const auto synced = engine.complete_sync(complete);
  CF_REQUIRE(synced.has_value());
  CF_EXPECT(synced.value().state == coherence::SyncState::Completed);
  CF_EXPECT(engine.get_region(device_record.value().id).value().state ==
            coherence::CoherenceState::Current);

  context.phase("DEVICE_WRITE_AUTHORITY");
  coherence::WriteRequest device_write;
  device_write.context = device_context;
  device_write.region = device_record.value().id;
  device_write.region_generation = device_record.value().generation;
  const auto device_grant = engine.acquire_write(device_write);
  CF_REQUIRE(device_grant.has_value());
  CF_EXPECT(device_grant.value().granted);
  // The host replica must be invalidated before the device may mutate.
  CF_EXPECT(!device_grant.value().may_mutate_now);
  CF_REQUIRE(device_grant.value().required_invalidations.size() == 1);
  const coherence::InvalidationRecord invalidation =
      device_grant.value().required_invalidations.front();
  CF_EXPECT(invalidation.region == host_record.value().id);
  coherence::InvalidationAck ack;
  ack.context = authority;
  ack.invalidation = invalidation.id;
  ack.region = invalidation.region;
  ack.region_generation = invalidation.region_generation;
  ack.replica_generation = invalidation.replica_generation;
  ack.superseded_version = invalidation.superseded_version;
  CF_REQUIRE(engine.acknowledge_invalidation(ack).has_value());
  const auto effective = engine.acquire_write(device_write);
  CF_REQUIRE(effective.has_value());
  CF_EXPECT(effective.value().may_mutate_now);
  CF_EXPECT(engine.get_region(host_record.value().id).value().state ==
            coherence::CoherenceState::Stale);

  context.phase("KERNEL_MUTATION");
  CF_REQUIRE(buffer.value().mutate(kSeed, kDelta).ok());
  coherence::DirtyRequest dirty;
  dirty.context = device_context;
  dirty.region = device_record.value().id;
  dirty.region_generation = device_record.value().generation;
  dirty.ownership_generation = effective.value().context.ownership_generation;
  dirty.base_version = first.value().version;
  CF_REQUIRE(engine.mark_dirty(dirty).has_value());
  auto mutated = buffer.value().download();
  CF_REQUIRE(mutated.has_value());
  const coherence::ContentFingerprint device_content =
      coherence::fingerprint_bytes(coherence::ByteSpan(mutated.value()));

  context.phase("PUBLISH_V2");
  coherence::PublishRequest publish_second;
  publish_second.context = device_context;
  publish_second.ownership_generation = effective.value().context.ownership_generation;
  publish_second.region = device_record.value().id;
  publish_second.region_generation = device_record.value().generation;
  publish_second.expected_base_version = first.value().version;
  publish_second.content = device_content;
  const auto second = engine.publish(publish_second);
  CF_REQUIRE(second.has_value());
  CF_EXPECT(second.value().state == coherence::PublicationState::Committed);
  CF_EXPECT(second.value().version == first.value().version.next());
  // The publication fingerprint is the fingerprint of the real device bytes.
  CF_EXPECT(second.value().content == device_content);

  context.phase("HOST_MUST_RESYNC");
  coherence::ReadRequest host_read;
  host_read.context = authority;
  host_read.region = host_record.value().id;
  host_read.require_current = true;
  const auto decision = engine.acquire_read(host_read);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().outcome != coherence::ReadOutcome::ReadCurrent);

  context.phase("D2H_RESYNC");
  coherence::SyncRequest resync;
  resync.context = authority;
  resync.source_region = device_record.value().id;
  resync.source_region_generation = device_record.value().generation;
  resync.destination_region = host_record.value().id;
  resync.destination_region_generation = host_record.value().generation;
  resync.transport = "cuda_memcpy";
  const auto resync_plan = engine.begin_sync(resync);
  CF_REQUIRE(resync_plan.has_value());
  auto refreshed = buffer.value().download();
  CF_REQUIRE(refreshed.has_value());
  CF_REQUIRE(store.fill(host_region.value(), 0, 0, 0).ok());
  std::memcpy(store.at(host_region.value()).bytes().data(), refreshed.value().data(),
              static_cast<std::size_t>(kExtent));
  const coherence::ContentFingerprint host_content =
      store.fingerprint(host_region.value()).value();
  CF_EXPECT(host_content == device_content);
  coherence::SyncCompleteRequest resync_complete;
  resync_complete.context = authority;
  resync_complete.operation = resync_plan.value().operation;
  resync_complete.destination_region = host_record.value().id;
  resync_complete.destination_region_generation = host_record.value().generation;
  resync_complete.destination_new_version = second.value().version;
  resync_complete.observed_content = host_content;
  const auto resynced = engine.complete_sync(resync_complete);
  CF_REQUIRE(resynced.has_value());
  CF_EXPECT(resynced.value().state == coherence::SyncState::Completed);
  CF_EXPECT(engine.get_region(host_record.value().id).value().state ==
            coherence::CoherenceState::Current);
  const auto final_read = engine.acquire_read(host_read);
  CF_REQUIRE(final_read.has_value());
  CF_EXPECT(final_read.value().outcome == coherence::ReadOutcome::ReadCurrent);
  CF_EXPECT(final_read.value().authoritative_version == second.value().version);

  context.phase("CLEANUP");
  CF_EXPECT(engine.audit().clean);
  CF_REQUIRE(buffer.value().synchronize().ok());
  buffer.value() = coherence::adapters::CudaBuffer{};
}
CF_TEST_MAIN()

