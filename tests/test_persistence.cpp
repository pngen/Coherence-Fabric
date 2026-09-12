// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Durable state coverage: framing integrity, corruption and truncation
// rejection, atomic replacement, real process exit followed by reload, and
// conservative recovery of in-flight publications.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "coherence/platform_file.hpp"
#include "process_util.hpp"
#include "test_framework.hpp"

namespace {

std::filesystem::path scratch(const std::string& name) {
  const std::filesystem::path base = std::filesystem::current_path() / "persistence-scratch" / name;
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);
  return base;
}

std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
  auto read = coherence::read_entire_file(path, 1ull << 30);
  if (read.has_value()) return read.value().bytes;
  std::printf("MARK read_file_bytes failed path=%s status=%s\n", path.string().c_str(),
              read.status().to_string().c_str());
  std::fflush(stdout);
  return std::vector<std::byte>{};
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  (void)coherence::atomic_replace_file(path, coherence::ByteSpan(bytes));
}

coherence::DurableImage sample_image() {
  coherence::DurableImage image;
  image.sequence = 12;
  image.epoch = coherence::CoordinatorEpoch::from_value(3);

  coherence::CoherencePolicy policy = coherence::strict_policy("durable-policy");
  policy.id = coherence::PolicyId::from_value(1);
  policy.generation = coherence::PolicyGeneration::from_value(1);
  image.policies.emplace(policy.id, policy);

  coherence::DomainRecord domain;
  domain.id = coherence::CoherenceDomainId::from_value(1);
  domain.name = "durable";
  domain.generation = 1;
  domain.lifecycle = coherence::DomainLifecycle::Active;
  domain.created_epoch = image.epoch;
  domain.current_epoch = image.epoch;
  image.domains.emplace(domain.id, domain);

  coherence::ParticipantRecord participant;
  participant.id = coherence::ParticipantId::from_value(1);
  participant.name = "writer";
  participant.boot = coherence::ParticipantBootId::from_value(coherence::UInt128{4, 4});
  participant.lifecycle = coherence::ParticipantLifecycle::Active;
  participant.admitted_epoch = image.epoch;
  participant.last_epoch = image.epoch;
  participant.region_count = 1;
  image.participants.emplace(participant.id, participant);

  coherence::ObjectRecord object;
  object.id = coherence::ObjectId::from_value(1);
  object.generation = coherence::ObjectGeneration::from_value(1);
  object.domain = domain.id;
  object.name = "object";
  object.length = 4096;
  object.policy = policy.id;
  object.policy_generation = policy.generation;
  object.lifecycle = coherence::ObjectLifecycle::Active;
  object.ownership_generation = coherence::OwnershipGeneration::from_value(2);
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
  region.domain = domain.id;
  region.object = object.id;
  region.object_generation = object.generation;
  region.participant = participant.id;
  region.boot = participant.boot;
  region.memory_domain = coherence::MemoryDomain::HostPageable;
  region.evidence_class = coherence::EvidenceClass::Real;
  region.name = "replica";
  region.offset = 0;
  region.length = 4096;
  region.lifecycle = coherence::RegionLifecycle::Active;
  region.state = coherence::CoherenceState::Current;
  region.version = coherence::VersionId::from_value(1);
  region.ownership_generation = object.ownership_generation;
  image.regions.emplace(region.id, region);
  return image;
}

coherence::Status write_sample_state(const std::filesystem::path& directory) {
  // One store, one handle: a second handle for the same journal in the same
  // process is a sharing violation and is refused by design.
  coherence::CoherenceEngine engine;
  auto attached = engine.attach_store(
      std::make_shared<coherence::FileDurableStore>(directory));
  if (!attached.has_value()) return attached.status();
  auto domain = engine.create_domain("durable");
  if (!domain.has_value()) return domain.status();
  auto object = engine.register_object(domain.value().id, "object", 4096,
                                       coherence::strict_policy("p"));
  if (!object.has_value()) return object.status();
  auto participant = engine.register_participant("writer",
                                                 coherence::generate_participant_boot_id(),
                                                 engine.epoch(), "node");
  if (!participant.has_value()) return participant.status();
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
  auto region = engine.register_region(registration);
  if (!region.has_value()) return region.status();
  coherence::RevalidateRequest revalidate;
  revalidate.context = registration.context;
  revalidate.region = region.value().id;
  revalidate.region_generation = region.value().generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content.length = 4096;
  revalidate.content.defined = true;
  revalidate.content.crc32c = 0x1234;
  if (!engine.revalidate_region(revalidate).has_value()) {
    return coherence::Status(coherence::StatusCode::InternalError, "revalidation failed");
  }
  coherence::WriteRequest write;
  write.context = registration.context;
  write.region = region.value().id;
  write.region_generation = region.value().generation;
  auto grant = engine.acquire_write(write);
  if (!grant.has_value()) return grant.status();
  coherence::PublishRequest publish;
  publish.context = registration.context;
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region.value().id;
  publish.region_generation = region.value().generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = revalidate.content;
  auto receipt = engine.publish(publish);
  if (!receipt.has_value()) return receipt.status();
  (void)engine.flush_durable();
  return coherence::Status::success();
}

} // namespace

CF_TEST(durable_store_round_trips_through_files) {
  context.phase("SETUP");
  const std::filesystem::path directory = scratch("round-trip");
  coherence::FileDurableStore store(directory);
  coherence::DurableImage empty;
  auto opened = store.open(empty);
  CF_REQUIRE(opened.has_value());
  CF_EXPECT(!opened.value().snapshot_loaded);

  coherence::DurableImage image = sample_image();
  {
    coherence::JournalEntry entry;
    entry.kind = coherence::JournalEntryKind::UpsertObject;
    entry.object = image.objects.at(coherence::ObjectId::from_value(1));
    CF_REQUIRE(store.append(entry).ok());
    entry.kind = coherence::JournalEntryKind::UpsertRegion;
    entry.region = image.regions.at(coherence::RegionId::from_value(1));
    CF_REQUIRE(store.append(entry).ok());
    entry.kind = coherence::JournalEntryKind::UpsertDomain;
    entry.domain = image.domains.at(coherence::CoherenceDomainId::from_value(1));
    CF_REQUIRE(store.append(entry).ok());
    entry.kind = coherence::JournalEntryKind::UpsertParticipant;
    entry.participant = image.participants.at(coherence::ParticipantId::from_value(1));
    CF_REQUIRE(store.append(entry).ok());
    entry.kind = coherence::JournalEntryKind::UpsertPolicy;
    entry.policy = image.policies.at(coherence::PolicyId::from_value(1));
    CF_REQUIRE(store.append(entry).ok());
  }
  CF_REQUIRE(store.compact(image).ok());
  CF_REQUIRE(store.close().ok());

  context.phase("RELOAD");
  coherence::FileDurableStore reopened(directory);
  coherence::DurableImage loaded;
  auto reloaded = reopened.open(loaded);
  CF_REQUIRE(reloaded.has_value());
  CF_EXPECT(reloaded.value().snapshot_loaded);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(loaded.objects.size()), 1u);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(loaded.regions.size()), 1u);
  CF_EXPECT(loaded.epoch == image.epoch);
  CF_REQUIRE(reopened.close().ok());

  // Atomic replacement leaves no temporary files behind.
  int entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    (void)entry;
    ++entries;
  }
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(entries), 2u);
}

CF_TEST(snapshot_corruption_and_truncation_are_rejected) {
  context.phase("SETUP");
  const std::filesystem::path directory = scratch("corruption");
  const std::vector<std::byte> good = coherence::encode_snapshot(sample_image());
  coherence::FileDurableStore store(directory);
  coherence::DurableImage ignored;
  CF_REQUIRE(store.open(ignored).has_value());

  context.phase("CORRUPT");
  for (std::size_t index = 0; index < good.size(); index += 5) {
    std::vector<std::byte> damaged = good;
    damaged[index] =
        static_cast<std::byte>(std::to_integer<std::uint8_t>(damaged[index]) ^ 0x5Au);
    write_file_bytes(store.snapshot_path(), damaged);
    coherence::FileDurableStore probe(directory);
    coherence::DurableImage image;
    const auto opened = probe.open(image);
    CF_EXPECT(!opened.has_value());
    if (opened.has_value()) (void)probe.close();
  }

  context.phase("TRUNCATE");
  for (std::size_t length = 1; length < good.size(); length += 11) {
    std::vector<std::byte> shortened(good.begin(),
                                     good.begin() + static_cast<std::ptrdiff_t>(length));
    write_file_bytes(store.snapshot_path(), shortened);
    coherence::FileDurableStore probe(directory);
    coherence::DurableImage image;
    CF_EXPECT(!probe.open(image).has_value());
  }

  context.phase("TRAILING");
  std::vector<std::byte> extended = good;
  extended.push_back(std::byte{0x7F});
  write_file_bytes(store.snapshot_path(), extended);
  coherence::FileDurableStore probe(directory);
  coherence::DurableImage image;
  const auto opened = probe.open(image);
  CF_EXPECT(!opened.has_value());
  CF_REQUIRE(!opened.has_value());
  CF_EXPECT(opened.status().code() == coherence::StatusCode::TrailingGarbage);

  // A snapshot larger than the configured bound is refused rather than read.
  coherence::StoreLimits limits;
  limits.max_snapshot_bytes = 64;
  CF_EXPECT(!coherence::decode_snapshot(coherence::ByteSpan(good), limits, image).ok());
}

CF_TEST(journal_replay_stops_at_the_first_damaged_record) {
  context.phase("SETUP");
  const std::filesystem::path directory = scratch("journal");
  coherence::FileDurableStore store(directory);
  coherence::DurableImage image;
  CF_REQUIRE(store.open(image).has_value());
  coherence::JournalEntry first;
  first.kind = coherence::JournalEntryKind::UpsertDomain;
  first.domain = sample_image().domains.at(coherence::CoherenceDomainId::from_value(1));
  CF_REQUIRE(store.append(first).ok());
  coherence::JournalEntry second;
  second.kind = coherence::JournalEntryKind::UpsertObject;
  second.object = sample_image().objects.at(coherence::ObjectId::from_value(1));
  CF_REQUIRE(store.append(second).ok());
  coherence::JournalEntry third;
  third.kind = coherence::JournalEntryKind::UpsertRegion;
  third.region = sample_image().regions.at(coherence::RegionId::from_value(1));
  CF_REQUIRE(store.append(third).ok());
  // The journal handle is closed before the file is read from outside the
  // store, which is also the ordering the store itself uses before it rotates
  // the journal.
  CF_REQUIRE(store.close().ok());
  const std::vector<std::byte> journal = read_file_bytes(store.journal_path());
  context.mark("journal bytes=" + std::to_string(journal.size()));
  CF_REQUIRE(!journal.empty());

  context.phase("TRUNCATE_TAIL");
  // A partial final record must not be applied, but earlier records must be.
  std::vector<std::byte> partial(journal.begin(), journal.end() - 3);
  write_file_bytes(store.journal_path(), partial);
  coherence::FileDurableStore probe(directory);
  coherence::DurableImage loaded;
  const auto opened = probe.open(loaded);
  CF_REQUIRE(opened.has_value());
  CF_EXPECT_EQ_U(opened.value().journal_records_replayed, 2u);
  CF_EXPECT_EQ_U(opened.value().corrupt_records, 1u);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(loaded.regions.size()), 0u);
  CF_REQUIRE(probe.close().ok());

  context.phase("CORRUPT_BODY");
  std::vector<std::byte> damaged = journal;
  damaged[damaged.size() - 1] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(damaged.back()) ^ 0xFFu);
  write_file_bytes(store.journal_path(), damaged);
  coherence::FileDurableStore second_probe(directory);
  coherence::DurableImage second_image;
  const auto second_opened = second_probe.open(second_image);
  CF_REQUIRE(second_opened.has_value());
  CF_EXPECT_EQ_U(second_opened.value().journal_records_replayed, 2u);
  CF_EXPECT_EQ_U(second_opened.value().corrupt_records, 1u);
  CF_REQUIRE(second_probe.close().ok());
}

CF_TEST(a_real_process_exit_then_reload_recovers_conservatively) {
  context.phase("SETUP");
  const std::filesystem::path directory = scratch("process-exit");
  const std::filesystem::path executable = cfproc::executable_path("cf_test_persistence");
  context.mark("executable=" + executable.string());
  CF_REQUIRE(std::filesystem::exists(executable));

  context.phase("CHILD");
  std::vector<std::string> transcript;
  const int code = cfproc::run_to_completion(executable, {"--write-and-exit", directory.string()},
                                             &transcript);
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(code < 0 ? 1u : 0u), 0u);
  if (code != 0) {
    for (const std::string& line : transcript) context.mark(line);
  }
  CF_REQUIRE(code == 0);

  context.phase("RECOVER");
  coherence::CoherenceEngine engine;
  auto store = std::make_shared<coherence::FileDurableStore>(directory);
  const auto recovered = engine.attach_store(store);
  CF_REQUIRE(recovered.has_value());
  CF_EXPECT(recovered.value().performed);
  CF_EXPECT(recovered.value().new_epoch.value() > recovered.value().previous_epoch.value());
  CF_EXPECT(recovered.value().objects_restored >= 1);
  CF_EXPECT(recovered.value().regions_restored >= 1);
  CF_EXPECT(recovered.value().regions_downgraded >= 1);

  const auto snapshot = engine.snapshot(coherence::SnapshotOptions{});
  CF_REQUIRE(snapshot.has_value());
  CF_REQUIRE(!snapshot.value().objects.empty());
  for (const coherence::ObjectRecord& object : snapshot.value().objects) {
    // Dynamic authority must not survive the restart.
    CF_EXPECT(!object.writer.defined());
    CF_EXPECT(object.authority == coherence::AuthorityMode::RevalidationRequired ||
              object.authority == coherence::AuthorityMode::None);
    CF_EXPECT(object.reads.empty());
  }
  for (const coherence::RegionRecord& region : snapshot.value().regions) {
    CF_EXPECT(region.state == coherence::CoherenceState::RevalidationRequired);
    CF_EXPECT(region.state != coherence::CoherenceState::Current);
    // The evidence attached after recovery is provenance, not proof.
    CF_EXPECT(region.evidence.defined());
  }
  // The published version itself is durable metadata and is restored.
  CF_EXPECT(snapshot.value().objects.front().authoritative_version.defined());
  CF_EXPECT(engine.audit().clean);
}

CF_TEST(a_durable_pending_publication_is_completed_under_its_own_identity) {
  context.phase("SETUP");
  coherence::DurableImage image = sample_image();
  const coherence::ObjectId object_id = coherence::ObjectId::from_value(1);
  const coherence::PublicationId publication = coherence::PublicationId::from_value(77);
  const coherence::VersionId pending_version = coherence::VersionId::from_value(2);
  const coherence::RequestId request = coherence::RequestId::from_value(0xABCDEF);
  coherence::ObjectRecord& object = image.objects.at(object_id);
  object.publication_state = coherence::PublicationState::PendingDurable;
  object.pending_publication = publication;
  object.pending_version = pending_version;
  object.has_unpublished_dirty = true;
  object.dirty_condition = coherence::DirtyCondition::DirtyUnpublished;
  object.writer = coherence::ParticipantId::from_value(1);
  object.writer_boot = coherence::ParticipantBootId::from_value(coherence::UInt128{4, 4});

  coherence::PendingPublication pending;
  pending.id = publication;
  pending.object = object_id;
  pending.object_generation = object.generation;
  pending.version = pending_version;
  pending.prior_version = coherence::VersionId::from_value(1);
  pending.ownership_generation = object.ownership_generation;
  pending.epoch = image.epoch;
  pending.writer = object.writer;
  pending.writer_boot = object.writer_boot;
  pending.request = request;
  pending.content.length = 4096;
  pending.content.defined = true;
  pending.content.crc32c = 0x99;
  pending.sequence = coherence::OperationSequence::from_value(5);
  image.pending_publications.emplace(publication, pending);

  auto store = std::make_shared<coherence::MemoryDurableStore>();
  store->set_snapshot_bytes(coherence::encode_snapshot(image));

  context.phase("RECOVER");
  coherence::CoherenceEngine engine;
  const auto recovered = engine.attach_store(store);
  CF_REQUIRE(recovered.has_value());
  CF_EXPECT_EQ_U(recovered.value().publications_completed, 1u);
  const auto restored = engine.get_object(object_id);
  CF_REQUIRE(restored.has_value());
  // Recovery converges on the exact publication identity that was durably
  // recorded rather than inventing a second version.
  CF_EXPECT(restored.value().authoritative_version == pending_version);
  CF_EXPECT(restored.value().publication == publication);
  CF_EXPECT(restored.value().publication_state == coherence::PublicationState::Committed);
  CF_EXPECT(restored.value().has_unpublished_dirty == false);
  CF_EXPECT(engine.audit().clean);
}

CF_TEST(a_conservative_recovery_policy_refuses_to_complete_a_pending_publication) {
  context.phase("SETUP");
  coherence::DurableImage image = sample_image();
  coherence::CoherencePolicy conservative = coherence::strict_policy("conservative");
  conservative.recovery_policy = coherence::RecoveryPolicy::Conservative;
  conservative.id = coherence::PolicyId::from_value(1);
  conservative.generation = coherence::PolicyGeneration::from_value(1);
  image.policies.at(coherence::PolicyId::from_value(1)) = conservative;
  const coherence::ObjectId object_id = coherence::ObjectId::from_value(1);
  const coherence::PublicationId publication = coherence::PublicationId::from_value(88);
  coherence::ObjectRecord& object = image.objects.at(object_id);
  object.publication_state = coherence::PublicationState::PendingDurable;
  object.pending_publication = publication;
  object.pending_version = coherence::VersionId::from_value(9);
  coherence::PendingPublication pending;
  pending.id = publication;
  pending.object = object_id;
  pending.version = coherence::VersionId::from_value(9);
  pending.prior_version = coherence::VersionId::from_value(1);
  pending.ownership_generation = object.ownership_generation;
  pending.epoch = image.epoch;
  pending.writer = coherence::ParticipantId::from_value(1);
  pending.writer_boot = coherence::ParticipantBootId::from_value(coherence::UInt128{4, 4});
  image.pending_publications.emplace(publication, pending);

  auto store = std::make_shared<coherence::MemoryDurableStore>();
  store->set_snapshot_bytes(coherence::encode_snapshot(image));
  coherence::CoherenceEngine engine;
  const auto recovered = engine.attach_store(store);
  CF_REQUIRE(recovered.has_value());
  CF_EXPECT_EQ_U(recovered.value().publications_requiring_operator, 1u);
  CF_EXPECT_EQ_U(recovered.value().publications_completed, 0u);
  const auto restored = engine.get_object(object_id);
  CF_REQUIRE(restored.has_value());
  CF_EXPECT(restored.value().authoritative_version == coherence::VersionId::from_value(1));
  CF_EXPECT(restored.value().publication_state == coherence::PublicationState::RecoveryRequired);
  CF_EXPECT(restored.value().lifecycle == coherence::ObjectLifecycle::RecoveryRequired);
  CF_EXPECT_AUDIT_CLEAN(engine.audit());
}

CF_TEST(a_failed_durable_append_never_publishes_a_version) {
  context.phase("SETUP");
  coherence::EngineConfig config;
  config.enable_durability = true;
  coherence::CoherenceEngine engine(config);
  auto store = std::make_shared<coherence::MemoryDurableStore>();
  CF_REQUIRE(engine.attach_store(store).has_value());
  const auto domain = engine.create_domain("fail");
  const auto object = engine.register_object(domain.value().id, "o", 4096,
                                            coherence::strict_policy("p"));
  const auto participant = engine.register_participant(
      "p", coherence::generate_participant_boot_id(), engine.epoch(), "");
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
  const auto region = engine.register_region(registration);
  CF_REQUIRE(region.has_value());
  coherence::RevalidateRequest revalidate;
  revalidate.context = registration.context;
  revalidate.region = region.value().id;
  revalidate.region_generation = region.value().generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content.length = 4096;
  revalidate.content.defined = true;
  revalidate.content.crc32c = 0x55;
  CF_REQUIRE(engine.revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = registration.context;
  write.region = region.value().id;
  write.region_generation = region.value().generation;
  const auto grant = engine.acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().granted);

  context.phase("INJECT");
  // Every subsequent durable append fails.
  store->fail_append_at(store->append_count() + 1);
  coherence::PublishRequest publish;
  publish.context = registration.context;
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region.value().id;
  publish.region_generation = region.value().generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = revalidate.content;
  const auto receipt = engine.publish(publish);
  CF_EXPECT(!receipt.has_value());
  CF_EXPECT(receipt.status().code() == coherence::StatusCode::PersistenceFailure);
  const auto object_after = engine.get_object(object.value().id);
  CF_REQUIRE(object_after.has_value());
  // No version became authoritative, and no reader can observe one.
  CF_EXPECT(!object_after.value().authoritative_version.defined());
  CF_EXPECT(object_after.value().publication_state == coherence::PublicationState::Aborted);
  CF_EXPECT(!object_after.value().pending_publication.defined());
  coherence::ReadRequest read;
  read.context = registration.context;
  const auto decision = engine.acquire_read(read);
  CF_REQUIRE(decision.has_value());
  CF_EXPECT(decision.value().outcome != coherence::ReadOutcome::ReadCurrent);
  CF_EXPECT(decision.value().reason == coherence::StatusCode::NotAuthoritative);
  // The durable store reports its own failure through the invariant auditor.
  CF_EXPECT(!engine.audit().clean);
}

int main(int argc, char** argv) {
  if (argc > 2 && std::strcmp(argv[1], "--write-and-exit") == 0) {
    const std::filesystem::path directory = argv[2];
    const coherence::Status written = write_sample_state(directory);
    if (!written.ok()) {
      std::fprintf(stderr, "child failed: %s\n", written.to_string().c_str());
      return 1;
    }
    // Exit without unwinding: this is a real process termination, so nothing
    // in-process can be relied upon to make the state valid.
    std::fflush(stdout);
    std::_Exit(0);
  }
  return cftest::run_suite(argc, argv);
}
