// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Real host-memory coherence proof.
//
// Every claim in this suite is checked against actual bytes: pageable host
// allocations, a second host replica, and a named shared mapping observed by
// two independent handles. The runtime is required to refuse to certify
// currentness when the bytes do not match, and the tests prove that refusal.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "coherence/adapters/region_store.hpp"
#include "coherence/bytes.hpp"
#include "coherence/engine.hpp"
#include "test_framework.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

struct MemoryFixture {
  coherence::EngineConfig config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::adapters::RegionStore store;
  coherence::ParticipantRecord participant;
  coherence::CoherenceDomainId domain;
  coherence::ObjectId object;

  MemoryFixture() {
    config.enable_durability = false;
    engine = std::make_unique<coherence::CoherenceEngine>(config);
    domain = engine->create_domain("memory").value().id;
    object = engine->register_object(domain, "buffer", 8192,
                                     coherence::strict_policy("memory-strict"))
                 .value().id;
    participant = engine->register_participant("host", coherence::generate_participant_boot_id(),
                                               engine->epoch(), "host-node")
                      .value();
  }

  coherence::AuthorityContext context() {
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

  coherence::RegionRecord add_region(const std::string& name, coherence::RegionId id,
                                     std::uint64_t offset, std::uint64_t length,
                                     coherence::MemoryDomain domain_tag) {
    coherence::RegionRegistration registration;
    registration.context = context();
    registration.domain = domain;
    registration.requested_id = id;
    registration.name = name;
    registration.memory_domain = domain_tag;
    const coherence::adapters::StoredRegion& stored = store.at(id);
    registration.evidence_class = stored.evidence_class;
    registration.offset = offset;
    registration.length = length;
    registration.address_hint = stored.address_hint;
    registration.content = coherence::fingerprint_bytes(stored.bytes());
    return engine->register_region(registration).value();
  }
};

bool try_pin_host_memory() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  void* block = ::VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE);
  if (block == nullptr) return false;
  const bool locked = ::VirtualLock(block, info.dwPageSize * 2) != 0;
  if (locked) ::VirtualUnlock(block, info.dwPageSize * 2);
  ::VirtualFree(block, 0, MEM_RELEASE);
  return locked;
#else
  return false;
#endif
}

} // namespace

CF_TEST(real_pageable_buffers_hold_and_verify_bytes) {
  context.phase("SETUP");
  MemoryFixture fixture;
  const auto first = fixture.store.add_host_region("a", 4096,
                                                   coherence::MemoryDomain::HostPageable, 1);
  const auto second = fixture.store.add_host_region("b", 4096,
                                                    coherence::MemoryDomain::HostPageable, 2);
  CF_REQUIRE(first.has_value() && second.has_value());
  const coherence::adapters::StoredRegion& a = fixture.store.at(first.value());
  const coherence::adapters::StoredRegion& b = fixture.store.at(second.value());
  CF_EXPECT_EQ_U(a.bytes().size(), 4096u);
  CF_EXPECT_EQ_U(b.bytes().size(), 4096u);
  CF_EXPECT(a.bytes().data() != b.bytes().data());
  // Two independently seeded buffers must differ.
  const coherence::ContentFingerprint fa = coherence::fingerprint_bytes(a.bytes());
  const coherence::ContentFingerprint fb = coherence::fingerprint_bytes(b.bytes());
  CF_EXPECT(!(fa == fb));

  context.phase("WRITE");
  CF_REQUIRE(fixture.store.poke(first.value(), 0, 0x5A, 8).ok());
  for (std::size_t i = 0; i < 8; ++i) {
    CF_EXPECT_EQ_U(std::to_integer<std::uint8_t>(a.bytes()[i]), 0x5Au);
  }
  CF_EXPECT(std::to_integer<std::uint8_t>(b.bytes()[0]) != 0x5Au);
}

CF_TEST(a_real_byte_copy_is_verified_by_content_comparison) {
  context.phase("SETUP");
  MemoryFixture fixture;
  const auto source = fixture.store.add_host_region("source", 8192,
                                                    coherence::MemoryDomain::HostPageable, 3);
  const auto destination = fixture.store.add_host_region(
      "destination", 8192, coherence::MemoryDomain::HostPageable, 4);
  CF_REQUIRE(source.has_value() && destination.has_value());
  const coherence::ContentFingerprint before =
      fixture.store.fingerprint(destination.value()).value();
  CF_REQUIRE(fixture.store.copy_and_verify(source.value(), destination.value()).ok());
  const coherence::ContentFingerprint after =
      fixture.store.fingerprint(destination.value()).value();
  CF_EXPECT(!(before == after));
  CF_EXPECT(fixture.store.fingerprint(source.value()).value() == after);

  // The copy refuses to certify an aliasing pair, because nothing moved.
  const coherence::Status aliased =
      fixture.store.copy_and_verify(source.value(), source.value());
  CF_EXPECT(!aliased.ok());
  CF_EXPECT(aliased.code() == coherence::StatusCode::InvalidArgument);
}

CF_TEST(a_single_changed_byte_invalidates_the_fingerprint) {
  context.phase("SETUP");
  MemoryFixture fixture;
  const auto region = fixture.store.add_host_region("r", 4096,
                                                    coherence::MemoryDomain::HostPageable, 5);
  CF_REQUIRE(region.has_value());
  const coherence::ContentFingerprint original =
      fixture.store.fingerprint(region.value()).value();
  CF_REQUIRE(fixture.store.poke(region.value(), 1000, 0x77).ok());
  const coherence::ContentFingerprint modified =
      fixture.store.fingerprint(region.value()).value();
  CF_EXPECT(!(original == modified));
  CF_EXPECT(original.crc32c != modified.crc32c);
}

CF_TEST(shared_mapping_is_observed_by_two_independent_handles) {
  context.phase("SETUP");
  MemoryFixture fixture;
  const std::string segment_name = "cf-test-shared-" + coherence::generate_request_id().to_string();
  auto created = fixture.store.add_shared_region("writer", segment_name, 4096, true);
  CF_REQUIRE(created.has_value());
  // A second, independent mapping of the same name observes the same pages.
  auto observer = coherence::adapters::SharedSegment::open(segment_name, 4096, false);
  CF_REQUIRE(observer.has_value());
  context.phase("WRITE");
  CF_REQUIRE(fixture.store.poke(created.value(), 64, 0xC3, 16).ok());
  const coherence::adapters::StoredRegion& writer = fixture.store.at(created.value());
  // Two views of one named mapping may have different base addresses in the
  // same process while describing the same physical pages, so identity is
  // established by observing the bytes rather than by comparing pointers.
  CF_EXPECT(!writer.bytes().empty());
  for (std::size_t i = 0; i < 16; ++i) {
    CF_EXPECT_EQ_U(std::to_integer<std::uint8_t>(observer.value().span()[64 + i]), 0xC3u);
  }
  // Creating a segment that already exists is refused rather than silently
  // reusing another run's bytes.
  auto duplicate = coherence::adapters::SharedSegment::open(segment_name, 4096, true);
  CF_EXPECT(!duplicate.has_value());
  CF_EXPECT(duplicate.status().code() == coherence::StatusCode::DuplicateIdentity);
}

CF_TEST(coherence_metadata_transitions_track_real_bytes) {
  context.phase("SETUP");
  MemoryFixture fixture;
  const auto a = fixture.store.add_host_region("a", 4096, coherence::MemoryDomain::HostPageable, 10);
  const auto b = fixture.store.add_host_region("b", 4096, coherence::MemoryDomain::HostPageable, 11);
  CF_REQUIRE(a.has_value() && b.has_value());
  const coherence::RegionRecord region_a =
      fixture.add_region("a", a.value(), 0, 4096, coherence::MemoryDomain::HostPageable);
  const coherence::RegionRecord region_b =
      fixture.add_region("b", b.value(), 4096, 4096, coherence::MemoryDomain::HostPageable);

  context.phase("PUBLISH");
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context();
  revalidate.region = region_a.id;
  revalidate.region_generation = region_a.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fixture.store.fingerprint(a.value()).value();
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context();
  write.region = region_a.id;
  write.region_generation = region_a.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context();
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region_a.id;
  publish.region_generation = region_a.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fixture.store.fingerprint(a.value()).value();
  const auto receipt = fixture.engine->publish(publish);
  CF_REQUIRE(receipt.has_value());
  // The publication fingerprint is the fingerprint of the real published bytes.
  CF_EXPECT(receipt.value().content == fixture.store.fingerprint(a.value()).value());

  context.phase("SYNC");
  coherence::SyncRequest sync;
  sync.context = fixture.context();
  sync.source_region = region_a.id;
  sync.source_region_generation = region_a.generation;
  sync.destination_region = region_b.id;
  sync.destination_region_generation = region_b.generation;
  sync.transport = "shared_mapping";
  const auto plan = fixture.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());
  CF_REQUIRE(fixture.store.copy_and_verify(a.value(), b.value()).ok());
  coherence::SyncCompleteRequest complete;
  complete.context = fixture.context();
  complete.operation = plan.value().operation;
  complete.destination_region = region_b.id;
  complete.destination_region_generation = region_b.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = fixture.store.fingerprint(b.value()).value();
  const auto completed = fixture.engine->complete_sync(complete);
  CF_REQUIRE(completed.has_value());
  CF_EXPECT(completed.value().state == coherence::SyncState::Completed);
  CF_EXPECT(fixture.engine->get_region(region_b.id).value().state ==
            coherence::CoherenceState::Current);

  context.phase("VERIFY");
  // The replica that is marked current really does hold the published bytes.
  const coherence::RegionRecord stored_region = fixture.engine->get_region(region_b.id).value();
  CF_EXPECT(stored_region.content == fixture.store.fingerprint(b.value()).value());
}

CF_TEST(a_replica_whose_bytes_were_tampered_with_cannot_become_current) {
  context.phase("SETUP");
  MemoryFixture fixture;
  const auto a = fixture.store.add_host_region("a", 4096, coherence::MemoryDomain::HostPageable, 20);
  const auto b = fixture.store.add_host_region("b", 4096, coherence::MemoryDomain::HostPageable, 21);
  CF_REQUIRE(a.has_value() && b.has_value());
  const coherence::RegionRecord region_a =
      fixture.add_region("a", a.value(), 0, 4096, coherence::MemoryDomain::HostPageable);
  const coherence::RegionRecord region_b =
      fixture.add_region("b", b.value(), 4096, 4096, coherence::MemoryDomain::HostPageable);

  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context();
  revalidate.region = region_a.id;
  revalidate.region_generation = region_a.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fixture.store.fingerprint(a.value()).value();
  CF_REQUIRE(fixture.engine->revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = fixture.context();
  write.region = region_a.id;
  write.region_generation = region_a.generation;
  const auto grant = fixture.engine->acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);
  coherence::PublishRequest publish;
  publish.context = fixture.context();
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region_a.id;
  publish.region_generation = region_a.generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = fixture.store.fingerprint(a.value()).value();
  CF_REQUIRE(fixture.engine->publish(publish).has_value());

  coherence::SyncRequest sync;
  sync.context = fixture.context();
  sync.source_region = region_a.id;
  sync.source_region_generation = region_a.generation;
  sync.destination_region = region_b.id;
  sync.destination_region_generation = region_b.generation;
  const auto plan = fixture.engine->begin_sync(sync);
  CF_REQUIRE(plan.has_value());

  context.phase("CORRUPT");
  // Move the bytes but then damage them, exactly as a bit-flip in transit or a
  // partial copy would.
  CF_REQUIRE(fixture.store.copy_and_verify(a.value(), b.value()).ok());
  CF_REQUIRE(fixture.store.poke(b.value(), 7, 0xFF).ok());
  coherence::SyncCompleteRequest complete;
  complete.context = fixture.context();
  complete.operation = plan.value().operation;
  complete.destination_region = region_b.id;
  complete.destination_region_generation = region_b.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = fixture.store.fingerprint(b.value()).value();
  const auto outcome = fixture.engine->complete_sync(complete);
  CF_REQUIRE(outcome.has_value());
  CF_EXPECT(outcome.value().state == coherence::SyncState::Failed);
  CF_EXPECT(outcome.value().reason == coherence::StatusCode::ContentMismatch);
  CF_EXPECT(fixture.engine->get_region(region_b.id).value().state ==
            coherence::CoherenceState::RevalidationRequired);
  CF_EXPECT(fixture.engine->audit().clean);
}

CF_TEST(pinned_host_memory_is_used_when_the_platform_supports_it) {
  context.phase("SETUP");
  if (!try_pin_host_memory()) {
    context.mark("pinned host memory is UNSUPPORTED on this platform build");
    CF_EXPECT(true);
    return;
  }
  MemoryFixture fixture;
  const auto pinned = fixture.store.add_host_region("pinned", 4096,
                                                    coherence::MemoryDomain::HostPinned, 30);
  CF_REQUIRE(pinned.has_value());
  const coherence::RegionRecord record =
      fixture.add_region("pinned", pinned.value(), 0, 4096, coherence::MemoryDomain::HostPinned);
  CF_EXPECT(record.memory_domain == coherence::MemoryDomain::HostPinned);
  CF_REQUIRE(fixture.store.poke(pinned.value(), 0, 0x11, 64).ok());
  coherence::RevalidateRequest revalidate;
  revalidate.context = fixture.context();
  revalidate.region = record.id;
  revalidate.region_generation = record.generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content = fixture.store.fingerprint(pinned.value()).value();
  const auto updated = fixture.engine->revalidate_region(revalidate);
  CF_REQUIRE(updated.has_value());
  CF_EXPECT(updated.value().state == coherence::CoherenceState::Current);
  CF_EXPECT(updated.value().evidence_class == coherence::EvidenceClass::Real);
}

CF_TEST(a_synthetic_cxl_region_is_labelled_synthetic) {
  context.phase("SETUP");
  MemoryFixture fixture;
  const auto synthetic = fixture.store.add_synthetic_cxl_region("cxl", 4096, 40);
  CF_REQUIRE(synthetic.has_value());
  const coherence::adapters::StoredRegion& stored = fixture.store.at(synthetic.value());
  // No physical CXL hardware is present, so the domain and the evidence class
  // must both say so.
  CF_EXPECT(stored.memory_domain == coherence::MemoryDomain::CxlClass);
  CF_EXPECT(stored.evidence_class == coherence::EvidenceClass::Synthetic);
  const coherence::RegionRecord record =
      fixture.add_region("cxl", synthetic.value(), 0, 4096, coherence::MemoryDomain::CxlClass);
  CF_EXPECT(record.evidence_class == coherence::EvidenceClass::Synthetic);
  CF_EXPECT(record.memory_domain == coherence::MemoryDomain::CxlClass);
}

CF_TEST_MAIN()

