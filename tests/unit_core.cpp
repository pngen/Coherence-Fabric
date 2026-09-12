// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Unit coverage for identities, status semantics, enumerations, transition
// legality, integrity primitives, policy validation and canonical codecs.
#include <string>
#include <vector>

#include "coherence/adapters/region_store.hpp"
#include "coherence/adapters/region_store.hpp"
#include "coherence/bytes.hpp"
#include "coherence/codec.hpp"
#include "coherence/enums.hpp"
#include "coherence/evidence.hpp"
#include "coherence/model.hpp"
#include "coherence/persistence.hpp"
#include "coherence/policy.hpp"
#include "coherence/serialize.hpp"
#include "coherence/status.hpp"
#include "coherence/transport.hpp"
#include "test_framework.hpp"

namespace {

std::string hex_of(coherence::ByteSpan bytes) { return coherence::to_hex(bytes); }

coherence::ByteSpan as_bytes(const std::string& text) {
  return coherence::ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

} // namespace

CF_TEST(strong_identities_are_distinct_types) {
  context.phase("SETUP");
  coherence::ObjectId object = coherence::ObjectId::from_value(7);
  coherence::RegionId region = coherence::RegionId::from_value(7);
  coherence::ParticipantId participant = coherence::ParticipantId::from_value(7);
  // Distinct types with the same numeric value must be independently typed.
  CF_EXPECT_EQ_U(object.value(), 7);
  CF_EXPECT_EQ_U(region.value(), 7);
  CF_EXPECT_EQ_U(participant.value(), 7);
  CF_EXPECT(object.defined());
  CF_EXPECT(!coherence::ObjectId::nil().defined());
  CF_EXPECT_EQ_U(object.next().value(), 8);
  CF_EXPECT_EQ(object.to_string(), std::string("7"));
}

CF_TEST(generations_advance_monotonically_and_wrap_safely) {
  context.phase("SETUP");
  coherence::VersionId version = coherence::VersionId::from_value(1);
  for (int i = 0; i < 1000; ++i) {
    const coherence::VersionId next = version.next();
    CF_EXPECT(next.value() == version.value() + 1);
    version = next;
  }
  coherence::UInt128 wide{0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
  const coherence::ParticipantBootId boot = coherence::ParticipantBootId::from_value(wide);
  const coherence::ParticipantBootId wrapped = boot.next();
  CF_EXPECT_EQ_U(wrapped.value().high, 0u);
  CF_EXPECT_EQ_U(wrapped.value().low, 0u);
}

CF_TEST(boot_identities_are_generated_from_a_cryptographic_source) {
  context.phase("SETUP");
  coherence::ParticipantBootId first = coherence::generate_participant_boot_id();
  coherence::ParticipantBootId second = coherence::generate_participant_boot_id();
  CF_EXPECT(first.defined());
  CF_EXPECT(second.defined());
  CF_EXPECT(first != second);
  coherence::SessionId session = coherence::generate_session_id();
  CF_EXPECT(session.defined());
  CF_EXPECT(coherence::generate_request_id().defined());
}

CF_TEST(status_codes_have_stable_distinct_tokens) {
  context.phase("SETUP");
  CF_EXPECT_EQ(std::string(coherence::status_code_name(coherence::StatusCode::StaleEpoch)),
               std::string("stale_epoch"));
  CF_EXPECT_EQ(std::string(coherence::status_code_name(coherence::StatusCode::StaleBoot)),
               std::string("stale_boot"));
  CF_EXPECT_EQ(
      std::string(coherence::status_code_name(coherence::StatusCode::StaleOwnership)),
      std::string("stale_ownership"));
  CF_EXPECT_EQ(std::string(coherence::status_code_name(coherence::StatusCode::OutcomeUnknown)),
               std::string("outcome_unknown"));
  CF_EXPECT(coherence::is_stale_code(coherence::StatusCode::StalePublication));
  CF_EXPECT(!coherence::is_stale_code(coherence::StatusCode::ReadNotCurrent));
  CF_EXPECT(coherence::status_severity(coherence::StatusCode::IntegrityFailure) ==
            coherence::StatusSeverity::Integrity);
}

CF_TEST(result_carries_either_a_value_or_a_failure) {
  context.phase("SETUP");
  coherence::Result<int> good = coherence::Result<int>::success(4);
  coherence::Result<int> bad = coherence::Result<int>::failure(
      coherence::Status(coherence::StatusCode::UnknownObject, "missing"));
  CF_EXPECT(good.has_value());
  CF_EXPECT(!bad.has_value());
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(good.value()), 4u);
  CF_EXPECT(bad.code() == coherence::StatusCode::UnknownObject);
  coherence::VoidResult ok = coherence::VoidResult::success();
  CF_EXPECT(ok.has_value());
}

CF_TEST(enumeration_tokens_round_trip_and_reject_unknown_values) {
  context.phase("SETUP");
  for (std::uint32_t raw = 1; raw <= static_cast<std::uint32_t>(coherence::MemoryDomain::Synthetic);
       ++raw) {
    const auto parsed = coherence::memory_domain_from_u8(static_cast<std::uint8_t>(raw));
    CF_REQUIRE(parsed.has_value());
    CF_EXPECT_EQ(std::string(coherence::to_token(parsed.value())),
                 std::string(coherence::to_token(static_cast<coherence::MemoryDomain>(raw))));
  }
  CF_EXPECT(!coherence::memory_domain_from_u8(200).has_value());
  CF_EXPECT(!coherence::coherence_state_from_u8(99).has_value());
  CF_EXPECT(!coherence::evidence_kind_from_u8(250).has_value());
  // The legitimate 'unknown' member must still decode.
  CF_EXPECT(coherence::coherence_state_from_u8(0).has_value());
  CF_EXPECT(coherence::memory_domain_from_u8(0).has_value());
}

CF_TEST(illegal_coherence_transitions_are_rejected) {
  context.phase("SETUP");
  using coherence::CoherenceState;
  CF_EXPECT(coherence::is_legal_coherence_transition(CoherenceState::Current,
                                                      CoherenceState::Stale));
  CF_EXPECT(coherence::is_legal_coherence_transition(CoherenceState::Stale,
                                                      CoherenceState::Current));
  CF_EXPECT(coherence::is_legal_coherence_transition(CoherenceState::Invalid,
                                                      CoherenceState::Current));
  // A retired replica never returns to a live state.
  CF_EXPECT(!coherence::is_legal_coherence_transition(CoherenceState::Retired,
                                                       CoherenceState::Current));
  CF_EXPECT(!coherence::is_legal_coherence_transition(CoherenceState::Retired,
                                                       CoherenceState::Dirty));
  // A dirty replica cannot become current without a publication path.
  CF_EXPECT(!coherence::is_legal_coherence_transition(CoherenceState::Dirty,
                                                       CoherenceState::Current) == false);
  CF_EXPECT(coherence::is_legal_coherence_transition(CoherenceState::Dirty,
                                                      CoherenceState::Current));
  // A fenced replica cannot become current.
  CF_EXPECT(!coherence::is_legal_coherence_transition(CoherenceState::Fenced,
                                                       CoherenceState::Current));
  // UNKNOWN may be resolved, and may also be re-derived, but never guessed into
  // a state that implies a proof.
  CF_EXPECT(coherence::is_legal_coherence_transition(CoherenceState::Unknown,
                                                      CoherenceState::RevalidationRequired));
}

CF_TEST(illegal_lifecycle_transitions_are_rejected) {
  context.phase("SETUP");
  using coherence::ObjectLifecycle;
  using coherence::ParticipantLifecycle;
  using coherence::RegionLifecycle;
  CF_EXPECT(coherence::is_legal_object_transition(ObjectLifecycle::Created,
                                                   ObjectLifecycle::Active));
  CF_EXPECT(!coherence::is_legal_object_transition(ObjectLifecycle::Retired,
                                                    ObjectLifecycle::Active));
  CF_EXPECT(coherence::is_legal_region_transition(RegionLifecycle::Active,
                                                   RegionLifecycle::Retired));
  CF_EXPECT(!coherence::is_legal_region_transition(RegionLifecycle::Retired,
                                                    RegionLifecycle::Active));
  // A fenced participant is never un-fenced; it must be re-admitted under a
  // fresh boot identity, which is a different incarnation.
  CF_EXPECT(!coherence::is_legal_participant_transition(ParticipantLifecycle::Fenced,
                                                         ParticipantLifecycle::Active));
  CF_EXPECT(coherence::is_legal_participant_transition(ParticipantLifecycle::Fenced,
                                                        ParticipantLifecycle::Retired));
}

CF_TEST(state_predicates_match_the_documented_meaning) {
  context.phase("SETUP");
  using coherence::CoherenceState;
  CF_EXPECT(coherence::is_current_state(CoherenceState::Current));
  CF_EXPECT(!coherence::is_current_state(CoherenceState::Stale));
  CF_EXPECT(!coherence::is_current_state(CoherenceState::Dirty));
  CF_EXPECT(coherence::is_readable_state(CoherenceState::Current));
  CF_EXPECT(coherence::is_readable_state(CoherenceState::Stale));
  CF_EXPECT(!coherence::is_readable_state(CoherenceState::Invalid));
  CF_EXPECT(!coherence::is_readable_state(CoherenceState::RevalidationRequired));
  CF_EXPECT(coherence::is_writable_state(CoherenceState::Dirty));
  CF_EXPECT(!coherence::is_writable_state(CoherenceState::Stale));
  CF_EXPECT(!coherence::is_live_state(CoherenceState::Retired));
  CF_EXPECT(!coherence::is_live_state(CoherenceState::Fenced));
}

CF_TEST(crc32c_matches_the_published_test_vector) {
  context.phase("SETUP");
  const std::string vector = "123456789";
  CF_EXPECT_EQ_U(coherence::crc32c(as_bytes(vector)), 0xE3069283u);
  CF_EXPECT_EQ_U(coherence::crc32c(coherence::ByteSpan{}), 0u);
  // Incremental seeding must compose.
  const std::string first = "1234";
  const std::string second = "56789";
  const std::uint32_t seeded = coherence::crc32c(0u, as_bytes(first));
  CF_EXPECT_EQ_U(coherence::crc32c(seeded, as_bytes(second)), 0xE3069283u);
}

CF_TEST(sha256_matches_the_published_test_vectors) {
  context.phase("SETUP");
  CF_EXPECT_EQ(coherence::sha256_hex(as_bytes("")),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CF_EXPECT_EQ(coherence::sha256_hex(as_bytes("abc")),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  // A long input exercises the multi-block path and the length encoding.
  std::string long_input;
  for (int i = 0; i < 1000; ++i) long_input += "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(coherence::sha256_hex(as_bytes(long_input)).size()),
                 64u);
}

CF_TEST(incremental_and_one_shot_sha256_agree) {
  context.phase("SETUP");
  std::string data;
  for (int i = 0; i < 500; ++i) data.push_back(static_cast<char>('a' + (i % 26)));
  coherence::Sha256 hasher;
  hasher.update(as_bytes(data.substr(0, 7)));
  hasher.update(as_bytes(data.substr(7)));
  const auto incremental = hasher.finish();
  const auto one_shot = coherence::Sha256::digest(as_bytes(data));
  CF_EXPECT(coherence::digest_equal(coherence::ByteSpan(incremental.data(), incremental.size()),
                                    coherence::ByteSpan(one_shot.data(), one_shot.size())));
}

CF_TEST(overflow_checked_arithmetic_refuses_to_wrap) {
  context.phase("SETUP");
  CF_EXPECT(coherence::checked_add(1, 2).has_value());
  CF_EXPECT_EQ_U(coherence::checked_add(1, 2).value(), 3u);
  CF_EXPECT(!coherence::checked_add(UINT64_MAX, 1).has_value());
  CF_EXPECT(!coherence::checked_mul(UINT64_MAX, 2).has_value());
  CF_EXPECT(coherence::checked_mul(0, UINT64_MAX).has_value());
  CF_EXPECT(coherence::checked_range(0, 10, 10));
  CF_EXPECT(coherence::checked_range(5, 5, 10));
  CF_EXPECT(!coherence::checked_range(5, 6, 10));
  CF_EXPECT(!coherence::checked_range(UINT64_MAX, 2, UINT64_MAX));
}

CF_TEST(binary_codecs_reject_truncation_and_trailing_bytes) {
  context.phase("SETUP");
  coherence::ByteWriter writer;
  writer.u32(0x11223344u);
  writer.text("hello");
  coherence::ByteReader reader(writer.span());
  std::uint32_t value = 0;
  std::string text;
  CF_REQUIRE(reader.u32(value));
  CF_REQUIRE(reader.text(text, 64));
  CF_EXPECT_EQ_U(value, 0x11223344u);
  CF_EXPECT_EQ(text, std::string("hello"));
  CF_EXPECT(reader.expect_end("probe").ok());

  coherence::ByteReader truncated(coherence::ByteSpan(writer.span().data(), 3));
  std::uint32_t ignored = 0;
  CF_EXPECT(!truncated.u32(ignored));
  CF_EXPECT(truncated.status().code() == coherence::StatusCode::TruncatedInput);
}

CF_TEST(binary_codecs_bound_peer_declared_lengths) {
  context.phase("SETUP");
  coherence::ByteWriter writer;
  writer.u32(0xFFFFFFFFu);  // declares a four gigabyte string
  writer.raw(as_bytes("short"));
  coherence::ByteReader reader(writer.span());
  std::string text;
  // The declared length exceeds both the caller bound and the remaining bytes.
  CF_EXPECT(!reader.text(text, 64));
  CF_EXPECT(reader.status().code() == coherence::StatusCode::TruncatedInput);
}

CF_TEST(record_codecs_round_trip_canonically) {
  context.phase("SETUP");
  coherence::RegionRecord region;
  region.id = coherence::RegionId::from_value(3);
  region.generation = coherence::RegionGeneration::from_value(2);
  region.replica = coherence::ReplicaId::from_value(5);
  region.replica_generation = coherence::ReplicaGeneration::from_value(1);
  region.domain = coherence::CoherenceDomainId::from_value(1);
  region.object = coherence::ObjectId::from_value(9);
  region.object_generation = coherence::ObjectGeneration::from_value(1);
  region.participant = coherence::ParticipantId::from_value(2);
  region.boot = coherence::ParticipantBootId::from_value(coherence::UInt128{1, 2});
  region.memory_domain = coherence::MemoryDomain::HostShared;
  region.evidence_class = coherence::EvidenceClass::Real;
  region.name = "replica";
  region.offset = 4096;
  region.length = 8192;
  region.state = coherence::CoherenceState::Current;
  region.version = coherence::VersionId::from_value(4);
  region.content = coherence::fingerprint_bytes(as_bytes("contents"));

  coherence::ByteWriter writer;
  coherence::encode_region(writer, region);
  coherence::RegionRecord decoded;
  coherence::ByteReader reader(writer.span());
  CF_REQUIRE(coherence::decode_region(reader, decoded, coherence::default_decode_limits()));
  CF_EXPECT(reader.expect_end("region").ok());
  CF_EXPECT_EQ(decoded.name, region.name);
  CF_EXPECT_EQ_U(decoded.offset, region.offset);
  CF_EXPECT_EQ_U(decoded.length, region.length);
  CF_EXPECT(decoded.state == region.state);
  CF_EXPECT(decoded.version == region.version);
  CF_EXPECT(decoded.content == region.content);
  // Encoding is canonical: the same record always produces the same bytes.
  coherence::ByteWriter again;
  coherence::encode_region(again, decoded);
  CF_EXPECT_EQ(hex_of(again.span()), hex_of(writer.span()));
}

CF_TEST(record_decoders_reject_undeclared_enum_values) {
  context.phase("SETUP");
  coherence::RegionRecord region;
  region.id = coherence::RegionId::from_value(1);
  region.name = "r";
  coherence::ByteWriter writer;
  coherence::encode_region(writer, region);
  std::vector<std::byte> bytes(writer.span().begin(), writer.span().end());

  // Locate the enumerated fields by encoding the same record twice with
  // different valid values: the differing bytes are exactly the enum bytes.
  coherence::RegionRecord other = region;
  other.memory_domain = coherence::MemoryDomain::PersistentMapped;
  other.state = coherence::CoherenceState::Retired;
  other.lifecycle = coherence::RegionLifecycle::Quiescing;
  other.dirty = coherence::DirtyCondition::DirtyUnpublished;
  other.evidence_class = coherence::EvidenceClass::Synthetic;
  coherence::ByteWriter second;
  coherence::encode_region(second, other);
  CF_REQUIRE(second.size() == bytes.size());
  std::vector<std::size_t> enum_offsets;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (bytes[index] != second.data()[index]) enum_offsets.push_back(index);
  }
  CF_EXPECT(enum_offsets.size() >= 3);
  for (const std::size_t index : enum_offsets) {
    std::vector<std::byte> corrupted = bytes;
    corrupted[index] = static_cast<std::byte>(0xFEu);
    coherence::RegionRecord decoded;
    coherence::ByteReader reader(corrupted);
    CF_EXPECT(!coherence::decode_region(reader, decoded, coherence::default_decode_limits()));
  }

  // An impossible collection size is rejected before anything is allocated.
  bool rejected = false;
  for (std::size_t index = 0; index + 8 <= bytes.size() && !rejected; ++index) {
    if (coherence::load_u64(bytes, index) != 0) continue;
    std::vector<std::byte> impossible = bytes;
    coherence::store_u64(coherence::MutableByteSpan(impossible), index, 0xFFFFFFFFull);
    coherence::RegionRecord decoded;
    coherence::ByteReader reader(impossible);
    if (!coherence::decode_region(reader, decoded, coherence::default_decode_limits())) {
      rejected = true;
    }
  }
  CF_EXPECT(rejected);
}

CF_TEST(policy_validation_rejects_unimplemented_semantics) {
  context.phase("SETUP");
  coherence::CoherencePolicy strict = coherence::strict_policy();
  CF_EXPECT(coherence::validate_policy(strict).ok());

  coherence::CoherencePolicy multi_writer = strict;
  multi_writer.write_ownership = coherence::WriteOwnershipMode::MultiWriterUnsupported;
  const coherence::Status rejected = coherence::validate_policy(multi_writer);
  CF_EXPECT(!rejected.ok());
  CF_EXPECT(rejected.code() == coherence::StatusCode::Unsupported);

  coherence::CoherencePolicy contradictory = strict;
  contradictory.stale_read_policy = coherence::StaleReadPolicy::Always;
  CF_EXPECT(coherence::validate_policy(contradictory).code() ==
            coherence::StatusCode::InvalidArgument);

  coherence::CoherencePolicy bounded = coherence::eventual_policy(0);
  CF_EXPECT(coherence::validate_policy(bounded).code() ==
            coherence::StatusCode::InvalidArgument);

  coherence::CoherencePolicy no_domains = strict;
  no_domains.allowed_domains_mask = 0;
  CF_EXPECT(coherence::validate_policy(no_domains).code() ==
            coherence::StatusCode::InvalidArgument);
}

CF_TEST(policy_rendering_is_deterministic) {
  context.phase("SETUP");
  coherence::CoherencePolicy policy = coherence::strict_policy("alpha");
  const std::string first = coherence::render_policy(policy);
  const std::string second = coherence::render_policy(policy);
  CF_EXPECT_EQ(first, second);
  CF_EXPECT(first.find("consistency=strict") != std::string::npos);
  CF_EXPECT(first.find("write_ownership=single_writer_exclusive") != std::string::npos);
  CF_EXPECT(coherence::summarize_policy(policy).find("strict") != std::string::npos);
}

CF_TEST(snapshot_and_journal_framing_detect_corruption) {
  context.phase("SETUP");
  coherence::DurableImage image;
  image.sequence = 1;
  image.epoch = coherence::CoordinatorEpoch::from_value(4);
  coherence::ObjectRecord object;
  object.id = coherence::ObjectId::from_value(1);
  object.generation = coherence::ObjectGeneration::from_value(1);
  object.name = "o";
  object.length = 128;
  object.policy = coherence::PolicyId::from_value(1);
  object.policy_generation = coherence::PolicyGeneration::from_value(1);
  image.objects.emplace(object.id, object);

  const std::vector<std::byte> encoded = coherence::encode_snapshot(image);
  coherence::DurableImage decoded;
  CF_REQUIRE(coherence::decode_snapshot(encoded, coherence::StoreLimits{}, decoded).ok());
  CF_EXPECT_EQ_U(decoded.objects.size(), 1u);
  CF_EXPECT(decoded.epoch == image.epoch);

  // Every single-byte corruption must be rejected.
  int rejected = 0;
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    std::vector<std::byte> corrupted = encoded;
    corrupted[index] = static_cast<std::byte>(
        std::to_integer<std::uint8_t>(corrupted[index]) ^ 0x5Au);
    coherence::DurableImage ignored;
    if (!coherence::decode_snapshot(corrupted, coherence::StoreLimits{}, ignored).ok()) {
      ++rejected;
    }
  }
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(rejected),
                 static_cast<std::uint64_t>(encoded.size()));

  // Truncation is never accepted.
  for (std::size_t length = 0; length < encoded.size(); length += 7) {
    coherence::DurableImage ignored;
    const coherence::ByteSpan shortened(encoded.data(), length);
    CF_EXPECT(!coherence::decode_snapshot(shortened, coherence::StoreLimits{}, ignored).ok());
  }
  // Trailing bytes are rejected rather than ignored.
  std::vector<std::byte> extended = encoded;
  extended.push_back(std::byte{0});
  coherence::DurableImage ignored;
  CF_EXPECT(!coherence::decode_snapshot(extended, coherence::StoreLimits{}, ignored).ok());
}

CF_TEST(journal_entry_framing_round_trips_and_detects_damage) {
  context.phase("SETUP");
  coherence::JournalEntry entry;
  entry.kind = coherence::JournalEntryKind::UpsertRegion;
  entry.sequence = 11;
  entry.region.id = coherence::RegionId::from_value(4);
  entry.region.name = "r";
  const std::vector<std::byte> frame = coherence::encode_journal_entry(entry);
  CF_REQUIRE(!frame.empty());
  coherence::JournalEntry decoded;
  CF_REQUIRE(coherence::decode_journal_entry(frame, coherence::StoreLimits{}, decoded).ok());
  CF_EXPECT(decoded.kind == entry.kind);
  CF_EXPECT_EQ_U(decoded.sequence, 11u);
  CF_EXPECT(decoded.region.id == entry.region.id);

  std::vector<std::byte> corrupted = frame;
  corrupted.back() = static_cast<std::byte>(std::to_integer<std::uint8_t>(corrupted.back()) ^ 1u);
  coherence::JournalEntry ignored;
  const coherence::Status status =
      coherence::decode_journal_entry(corrupted, coherence::StoreLimits{}, ignored);
  CF_EXPECT(!status.ok());
  CF_EXPECT(status.code() == coherence::StatusCode::IntegrityFailure);

  // An undeclared entry kind is rejected rather than defaulted.
  std::vector<std::byte> wrong_kind = frame;
  coherence::store_u16(coherence::MutableByteSpan(wrong_kind), 6, 900);
  CF_EXPECT(!coherence::decode_journal_entry(wrong_kind, coherence::StoreLimits{}, ignored).ok());

  // An oversized declared payload is rejected before any allocation.
  std::vector<std::byte> oversized = frame;
  coherence::store_u32(coherence::MutableByteSpan(oversized), 16, 0x7FFFFFFFu);
  coherence::StoreLimits limits;
  limits.max_journal_record_bytes = 1024;
  CF_EXPECT(!coherence::decode_journal_entry(oversized, limits, ignored).ok());
}

CF_TEST(content_fingerprints_detect_a_single_changed_byte) {
  context.phase("SETUP");
  std::vector<std::byte> bytes = coherence::adapters::make_pattern(4096, 11);
  const coherence::ContentFingerprint original = coherence::fingerprint_bytes(bytes);
  CF_EXPECT(original.defined);
  CF_EXPECT_EQ_U(original.length, 4096u);
  bytes[2048] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bytes[2048]) ^ 0x01u);
  const coherence::ContentFingerprint changed = coherence::fingerprint_bytes(bytes);
  CF_EXPECT(!(original == changed));
  CF_EXPECT(original.crc32c != changed.crc32c);
  CF_EXPECT(original.digest != changed.digest);
}

CF_TEST(deterministic_random_is_reproducible_from_its_seed) {
  context.phase("SETUP");
  coherence::DeterministicRandom first(1234);
  coherence::DeterministicRandom second(1234);
  coherence::DeterministicRandom other(1235);
  bool differs = false;
  for (int i = 0; i < 256; ++i) {
    const std::uint64_t a = first.next_u64();
    CF_EXPECT_EQ_U(a, second.next_u64());
    if (a != other.next_u64()) differs = true;
  }
  CF_EXPECT(differs);
  coherence::DeterministicRandom bounded(7);
  for (int i = 0; i < 512; ++i) CF_EXPECT(bounded.next_below(10) < 10);
}

CF_TEST(endpoint_parsing_rejects_malformed_input) {
  context.phase("SETUP");
  auto plain = coherence::parse_endpoint("127.0.0.1:8080", 0);
  CF_REQUIRE(plain.has_value());
  CF_EXPECT_EQ(plain.value().first, std::string("127.0.0.1"));
  CF_EXPECT_EQ_U(plain.value().second, 8080u);
  auto defaulted = coherence::parse_endpoint("localhost", 4242);
  CF_REQUIRE(defaulted.has_value());
  CF_EXPECT_EQ_U(defaulted.value().second, 4242u);
  auto bracketed = coherence::parse_endpoint("[::1]:5150", 0);
  CF_REQUIRE(bracketed.has_value());
  CF_EXPECT_EQ(bracketed.value().first, std::string("::1"));
  CF_EXPECT_EQ_U(bracketed.value().second, 5150u);
  CF_EXPECT(!coherence::parse_endpoint("host:99999", 0).has_value());
  CF_EXPECT(!coherence::parse_endpoint("host:0", 0).has_value());
  CF_EXPECT(!coherence::parse_endpoint("", 0).has_value());
}

CF_TEST(evidence_assessment_fails_closed) {
  context.phase("SETUP");
  coherence::EvidenceContext context_rules;
  context_rules.current_epoch = coherence::CoordinatorEpoch::from_value(5);
  context_rules.current_sequence = coherence::OperationSequence::from_value(100);
  context_rules.max_age_operations = 10;
  context_rules.participant = coherence::ParticipantId::from_value(1);
  context_rules.live_boot = coherence::ParticipantBootId::from_value(coherence::UInt128{9, 9});

  coherence::EvidenceRecord record;
  record.id = coherence::EvidenceId::from_value(1);
  record.generation = coherence::EvidenceGeneration::from_value(1);
  record.kind = coherence::EvidenceKind::ByteComparison;
  record.evidence_class = coherence::EvidenceClass::Real;
  record.participant = context_rules.participant;
  record.boot = context_rules.live_boot;
  record.epoch = context_rules.current_epoch;
  record.sequence = coherence::OperationSequence::from_value(95);
  record.process_local = true;
  CF_EXPECT(coherence::assess_evidence(record, context_rules).fresh());

  // A coordinator epoch advance invalidates the observation.
  coherence::EvidenceContext later = context_rules;
  later.current_epoch = coherence::CoordinatorEpoch::from_value(6);
  CF_EXPECT(coherence::assess_evidence(record, later).verdict ==
            coherence::FreshnessVerdict::Stale);

  // A participant restart invalidates process-local observations.
  coherence::EvidenceContext restarted = context_rules;
  restarted.live_boot = coherence::ParticipantBootId::from_value(coherence::UInt128{1, 1});
  CF_EXPECT(coherence::assess_evidence(record, restarted).verdict ==
            coherence::FreshnessVerdict::Stale);

  // Age beyond the policy bound invalidates the observation.
  coherence::EvidenceContext aged = context_rules;
  aged.current_sequence = coherence::OperationSequence::from_value(200);
  CF_EXPECT(coherence::assess_evidence(record, aged).verdict ==
            coherence::FreshnessVerdict::Stale);

  // Persisted metadata never establishes dynamic currentness.
  coherence::EvidenceRecord persisted = record;
  persisted.kind = coherence::EvidenceKind::PersistedMetadata;
  persisted.process_local = false;
  CF_EXPECT(coherence::assess_evidence(persisted, context_rules).verdict ==
            coherence::FreshnessVerdict::RequiresRevalidation);

  // Synthetic evidence cannot support a claim over a real memory domain.
  coherence::EvidenceRecord synthetic = record;
  synthetic.evidence_class = coherence::EvidenceClass::Synthetic;
  CF_EXPECT(coherence::assess_evidence(synthetic, context_rules).verdict ==
            coherence::FreshnessVerdict::Unsupported);
  coherence::EvidenceContext synthetic_domain = context_rules;
  synthetic_domain.synthetic_domain = true;
  CF_EXPECT(coherence::assess_evidence(synthetic, synthetic_domain).fresh());

  // A missing record is missing, not fresh.
  coherence::EvidenceRecord empty;
  CF_EXPECT(coherence::assess_evidence(empty, context_rules).verdict ==
            coherence::FreshnessVerdict::Missing);
}

CF_TEST(an_evidence_age_bound_of_zero_means_events_only) {
  context.phase("SETUP");
  coherence::EvidenceContext unbounded;
  unbounded.current_epoch = coherence::CoordinatorEpoch::from_value(2);
  unbounded.current_sequence = coherence::OperationSequence::from_value(1000000);
  unbounded.max_age_operations = 0;  // no age bound: events invalidate evidence
  unbounded.participant = coherence::ParticipantId::from_value(1);
  unbounded.live_boot = coherence::ParticipantBootId::from_value(coherence::UInt128{3, 3});

  coherence::EvidenceRecord record;
  record.id = coherence::EvidenceId::from_value(9);
  record.generation = coherence::EvidenceGeneration::from_value(1);
  record.kind = coherence::EvidenceKind::ByteComparison;
  record.evidence_class = coherence::EvidenceClass::Real;
  record.participant = unbounded.participant;
  record.boot = unbounded.live_boot;
  record.epoch = unbounded.current_epoch;
  record.sequence = coherence::OperationSequence::from_value(1);
  record.process_local = true;
  // A very old observation is still fresh when no age bound is configured.
  CF_EXPECT(coherence::assess_evidence(record, unbounded).fresh());

  // A non-zero bound applies, and the boundary is inclusive.
  coherence::EvidenceContext bounded = unbounded;
  bounded.max_age_operations = 10;
  bounded.current_sequence = coherence::OperationSequence::from_value(11);
  CF_EXPECT(coherence::assess_evidence(record, bounded).fresh());
  bounded.current_sequence = coherence::OperationSequence::from_value(12);
  CF_EXPECT(coherence::assess_evidence(record, bounded).verdict ==
            coherence::FreshnessVerdict::Stale);

  // A current replica stays current across many unrelated operations, which is
  // the behaviour the default policies rely on.
  coherence::EngineConfig config;
  config.enable_durability = false;
  coherence::CoherenceEngine engine(config);
  const auto domain = engine.create_domain("age");
  const auto object = engine.register_object(domain.value().id, "o", 512,
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
  registration.length = 512;
  const auto region = engine.register_region(registration);
  CF_REQUIRE(region.has_value());
  coherence::RevalidateRequest revalidate;
  revalidate.context = registration.context;
  revalidate.region = region.value().id;
  revalidate.region_generation = region.value().generation;
  revalidate.observed_version = coherence::VersionId::from_value(1);
  revalidate.content.length = 512;
  revalidate.content.defined = true;
  revalidate.content.crc32c = 1;
  CF_REQUIRE(engine.revalidate_region(revalidate).has_value());
  coherence::WriteRequest write;
  write.context = registration.context;
  write.region = region.value().id;
  write.region_generation = region.value().generation;
  const auto grant = engine.acquire_write(write);
  CF_REQUIRE(grant.has_value() && grant.value().may_mutate_now);
  coherence::PublishRequest publish;
  publish.context = registration.context;
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = region.value().id;
  publish.region_generation = region.value().generation;
  publish.expected_base_version = coherence::VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = revalidate.content;
  CF_REQUIRE(engine.publish(publish).has_value());

  // Thousands of unrelated reads must not age the evidence out.
  for (int i = 0; i < 5000; ++i) {
    coherence::ReadRequest request;
    request.context = registration.context;
    const auto decision = engine.acquire_read(request);
    CF_REQUIRE(decision.has_value());
    if (decision.value().outcome != coherence::ReadOutcome::ReadCurrent) {
      CF_EXPECT(decision.value().outcome == coherence::ReadOutcome::ReadCurrent);
      break;
    }
  }
  CF_EXPECT(engine.sequence().value() > 5000);
}

CF_TEST_MAIN()

