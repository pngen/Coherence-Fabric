// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/persistence.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

#include "coherence/codec.hpp"
#include "coherence/platform_file.hpp"
#include "coherence/version.hpp"

namespace coherence {
namespace {

constexpr std::uint32_t kPayloadCrcSeed = 0u;

void write_frame_header(MutableByteSpan header, std::uint16_t kind, std::uint64_t sequence,
                        std::uint32_t payload_length, std::uint32_t payload_crc) {
  store_u32(header, 0, kJournalRecordMagic);
  store_u16(header, 4, kJournalFormatVersion);
  store_u16(header, 6, kind);
  store_u64(header, 8, sequence);
  store_u32(header, 16, payload_length);
  store_u32(header, 20, payload_crc);
}

bool encode_payload(ByteWriter& w, const JournalEntry& entry) {
  switch (entry.kind) {
    case JournalEntryKind::Snapshot:
      return true;  // no payload: the snapshot file carries the image
    case JournalEntryKind::UpsertDomain:
      encode_domain(w, entry.domain);
      return true;
    case JournalEntryKind::UpsertParticipant:
      encode_participant(w, entry.participant);
      return true;
    case JournalEntryKind::UpsertObject:
      encode_object(w, entry.object);
      return true;
    case JournalEntryKind::UpsertRegion:
      encode_region(w, entry.region);
      return true;
    case JournalEntryKind::UpsertInvalidation:
      encode_invalidation(w, entry.invalidation);
      return true;
    case JournalEntryKind::UpsertSync:
      encode_sync(w, entry.sync);
      return true;
    case JournalEntryKind::UpsertPolicy:
      encode_policy(w, entry.policy);
      return true;
    case JournalEntryKind::SetPendingPublication:
      encode_pending_publication(w, entry.pending);
      return true;
    case JournalEntryKind::ClearPendingPublication:
      w.strong_id(entry.remove_object);
      return true;
    case JournalEntryKind::RemoveObject:
      w.strong_id(entry.remove_object);
      return true;
    case JournalEntryKind::RemoveRegion:
      w.strong_id(entry.remove_region);
      return true;
    case JournalEntryKind::RemoveSync:
      w.strong_id(entry.remove_sync);
      return true;
    case JournalEntryKind::RemoveParticipant:
      w.strong_id(entry.remove_participant);
      return true;
    case JournalEntryKind::SetEpoch:
      w.strong_id(entry.epoch);
      return true;
    case JournalEntryKind::Invalid:
      return false;
  }
  return false;
}

bool decode_payload(ByteReader& r, JournalEntry& entry, const DecodeLimits& limits) {
  switch (entry.kind) {
    case JournalEntryKind::Snapshot:
      return true;
    case JournalEntryKind::UpsertDomain:
      return decode_domain(r, entry.domain, limits);
    case JournalEntryKind::UpsertParticipant:
      return decode_participant(r, entry.participant, limits);
    case JournalEntryKind::UpsertObject:
      return decode_object(r, entry.object, limits);
    case JournalEntryKind::UpsertRegion:
      return decode_region(r, entry.region, limits);
    case JournalEntryKind::UpsertInvalidation:
      return decode_invalidation(r, entry.invalidation, limits);
    case JournalEntryKind::UpsertSync:
      return decode_sync(r, entry.sync, limits);
    case JournalEntryKind::UpsertPolicy:
      return decode_policy(r, entry.policy, limits);
    case JournalEntryKind::SetPendingPublication:
      return decode_pending_publication(r, entry.pending, limits);
    case JournalEntryKind::ClearPendingPublication:
      return r.strong_id(entry.remove_object);
    case JournalEntryKind::RemoveObject:
      return r.strong_id(entry.remove_object);
    case JournalEntryKind::RemoveRegion:
      return r.strong_id(entry.remove_region);
    case JournalEntryKind::RemoveSync:
      return r.strong_id(entry.remove_sync);
    case JournalEntryKind::RemoveParticipant:
      return r.strong_id(entry.remove_participant);
    case JournalEntryKind::SetEpoch:
      return r.strong_id(entry.epoch);
    case JournalEntryKind::Invalid:
      return false;
  }
  return false;
}

void encode_image_payload(ByteWriter& w, const DurableImage& image) {
  w.u64(image.sequence);
  w.strong_id(image.epoch);

  w.u64(image.domains.size());
  for (const auto& [id, record] : image.domains) {
    (void)id;
    encode_domain(w, record);
  }
  w.u64(image.participants.size());
  for (const auto& [id, record] : image.participants) {
    (void)id;
    encode_participant(w, record);
  }
  w.u64(image.policies.size());
  for (const auto& [id, record] : image.policies) {
    (void)id;
    encode_policy(w, record);
  }
  w.u64(image.objects.size());
  for (const auto& [id, record] : image.objects) {
    (void)id;
    encode_object(w, record);
  }
  w.u64(image.regions.size());
  for (const auto& [id, record] : image.regions) {
    (void)id;
    encode_region(w, record);
  }
  w.u64(image.invalidations.size());
  for (const auto& [id, record] : image.invalidations) {
    (void)id;
    encode_invalidation(w, record);
  }
  w.u64(image.syncs.size());
  for (const auto& [id, record] : image.syncs) {
    (void)id;
    encode_sync(w, record);
  }
  w.u64(image.pending_publications.size());
  for (const auto& [id, record] : image.pending_publications) {
    (void)id;
    encode_pending_publication(w, record);
  }
}

CoherenceDomainId record_key(const DomainRecord& r) { return r.id; }
ParticipantId record_key(const ParticipantRecord& r) { return r.id; }
PolicyId record_key(const CoherencePolicy& r) { return r.id; }
ObjectId record_key(const ObjectRecord& r) { return r.id; }
RegionId record_key(const RegionRecord& r) { return r.id; }
InvalidationId record_key(const InvalidationRecord& r) { return r.id; }
SyncOperationId record_key(const SyncRecord& r) { return r.id; }
PublicationId record_key(const PendingPublication& r) { return r.id; }

template <typename Map, typename Decode>
bool decode_collection(ByteReader& r, Map& out, const DecodeLimits& limits, Decode decode) {
  std::uint64_t count = 0;
  if (!r.u64(count)) return false;
  if (count > limits.max_collection) return false;
  for (std::uint64_t i = 0; i < count; ++i) {
    typename Map::mapped_type record;
    if (!decode(r, record, limits)) return false;
    out.emplace(record_key(record), std::move(record));
  }
  return true;
}

bool decode_image_payload(ByteReader& r, DurableImage& image, const DecodeLimits& limits) {
  if (!r.u64(image.sequence)) return false;
  if (!r.strong_id(image.epoch)) return false;

  if (!decode_collection(r, image.domains, limits, decode_domain)) return false;
  if (!decode_collection(r, image.participants, limits, decode_participant)) return false;
  if (!decode_collection(r, image.policies, limits, decode_policy)) return false;
  if (!decode_collection(r, image.objects, limits, decode_object)) return false;
  if (!decode_collection(r, image.regions, limits, decode_region)) return false;
  if (!decode_collection(r, image.invalidations, limits, decode_invalidation)) return false;
  if (!decode_collection(r, image.syncs, limits, decode_sync)) return false;
  if (!decode_collection(r, image.pending_publications, limits, decode_pending_publication)) {
    return false;
  }
  return true;
}

} // namespace

std::string_view to_token(JournalEntryKind value) noexcept {
  switch (value) {
    case JournalEntryKind::Invalid: return "invalid";
    case JournalEntryKind::Snapshot: return "snapshot";
    case JournalEntryKind::UpsertDomain: return "upsert_domain";
    case JournalEntryKind::UpsertParticipant: return "upsert_participant";
    case JournalEntryKind::UpsertObject: return "upsert_object";
    case JournalEntryKind::UpsertRegion: return "upsert_region";
    case JournalEntryKind::UpsertInvalidation: return "upsert_invalidation";
    case JournalEntryKind::UpsertSync: return "upsert_sync";
    case JournalEntryKind::UpsertPolicy: return "upsert_policy";
    case JournalEntryKind::SetPendingPublication: return "set_pending_publication";
    case JournalEntryKind::ClearPendingPublication: return "clear_pending_publication";
    case JournalEntryKind::RemoveObject: return "remove_object";
    case JournalEntryKind::RemoveRegion: return "remove_region";
    case JournalEntryKind::RemoveSync: return "remove_sync";
    case JournalEntryKind::RemoveParticipant: return "remove_participant";
    case JournalEntryKind::SetEpoch: return "set_epoch";
  }
  return "unknown";
}

std::optional<JournalEntryKind> journal_kind_from_u16(std::uint16_t raw) noexcept {
  switch (raw) {
    case 1: return JournalEntryKind::Snapshot;
    case 2: return JournalEntryKind::UpsertDomain;
    case 3: return JournalEntryKind::UpsertParticipant;
    case 4: return JournalEntryKind::UpsertObject;
    case 5: return JournalEntryKind::UpsertRegion;
    case 6: return JournalEntryKind::UpsertInvalidation;
    case 7: return JournalEntryKind::UpsertSync;
    case 8: return JournalEntryKind::UpsertPolicy;
    case 9: return JournalEntryKind::SetPendingPublication;
    case 10: return JournalEntryKind::ClearPendingPublication;
    case 11: return JournalEntryKind::RemoveObject;
    case 12: return JournalEntryKind::RemoveRegion;
    case 13: return JournalEntryKind::SetEpoch;
    case 14: return JournalEntryKind::RemoveSync;
    case 15: return JournalEntryKind::RemoveParticipant;
    default: return std::nullopt;
  }
}

std::string JournalEntry::describe() const {
  std::string out;
  out.append("journal_sequence=");
  out.append(std::to_string(sequence));
  out.append(" kind=");
  out.append(to_token(kind));
  switch (kind) {
    case JournalEntryKind::UpsertDomain:
      out.append(" domain=");
      out.append(domain.id.to_string());
      break;
    case JournalEntryKind::UpsertParticipant:
      out.append(" participant=");
      out.append(participant.id.to_string());
      break;
    case JournalEntryKind::UpsertObject:
      out.append(" object=");
      out.append(object.id.to_string());
      out.append(" version=");
      out.append(object.authoritative_version.to_string());
      break;
    case JournalEntryKind::UpsertRegion:
      out.append(" region=");
      out.append(region.id.to_string());
      break;
    case JournalEntryKind::UpsertInvalidation:
      out.append(" invalidation=");
      out.append(invalidation.id.to_string());
      break;
    case JournalEntryKind::UpsertSync:
      out.append(" sync=");
      out.append(sync.id.to_string());
      break;
    case JournalEntryKind::UpsertPolicy:
      out.append(" policy=");
      out.append(policy.id.to_string());
      break;
    case JournalEntryKind::SetPendingPublication:
      out.append(" publication=");
      out.append(pending.id.to_string());
      break;
    case JournalEntryKind::ClearPendingPublication:
    case JournalEntryKind::RemoveObject:
      out.append(" object=");
      out.append(remove_object.to_string());
      break;
    case JournalEntryKind::RemoveRegion:
      out.append(" region=");
      out.append(remove_region.to_string());
      break;
    case JournalEntryKind::RemoveSync:
      out.append(" sync=");
      out.append(remove_sync.to_string());
      break;
    case JournalEntryKind::RemoveParticipant:
      out.append(" participant=");
      out.append(remove_participant.to_string());
      break;
    case JournalEntryKind::SetEpoch:
      out.append(" epoch=");
      out.append(epoch.to_string());
      break;
    case JournalEntryKind::Snapshot:
    case JournalEntryKind::Invalid:
      break;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
std::vector<std::byte> encode_journal_entry(const JournalEntry& entry) {
  ByteWriter payload;
  if (!encode_payload(payload, entry)) {
    return {};
  }
  const std::uint32_t payload_length = static_cast<std::uint32_t>(payload.size());
  const std::uint32_t payload_crc = crc32c(kPayloadCrcSeed, payload.span());

  std::vector<std::byte> frame(kJournalHeaderSize + payload.size());
  write_frame_header(MutableByteSpan(frame).first(kJournalHeaderSize),
                     static_cast<std::uint16_t>(entry.kind), entry.sequence, payload_length,
                     payload_crc);
  std::memcpy(frame.data() + kJournalHeaderSize, payload.span().data(), payload.size());
  return frame;
}

Status decode_journal_entry(ByteSpan frame, const StoreLimits& limits, JournalEntry& out) {
  if (frame.size() < kJournalHeaderSize) {
    return Status(StatusCode::TruncatedInput, "journal frame is shorter than its header");
  }
  if (load_u32(frame, 0) != kJournalRecordMagic) {
    return Status(StatusCode::CorruptionDetected, "journal frame magic mismatch");
  }
  const std::uint16_t format = load_u16(frame, 4);
  if (format != kJournalFormatVersion) {
    return Status(StatusCode::UnsupportedSchema, "journal frame format version is not supported",
                  std::to_string(format));
  }
  const auto kind = journal_kind_from_u16(load_u16(frame, 6));
  if (!kind.has_value()) {
    return Status(StatusCode::ProtocolViolation, "journal frame carries an undeclared entry kind");
  }
  const std::uint64_t sequence = load_u64(frame, 8);
  const std::uint32_t payload_length = load_u32(frame, 16);
  const std::uint32_t payload_crc = load_u32(frame, 20);

  // Bound the declared length against both the configuration and the bytes
  // actually present before allocating or advancing.
  if (payload_length > limits.max_journal_record_bytes) {
    return Status(StatusCode::OversizedInput, "journal record exceeds the configured bound",
                  "declared=" + std::to_string(payload_length) +
                      " bound=" + std::to_string(limits.max_journal_record_bytes));
  }
  if (frame.size() < kJournalHeaderSize + payload_length) {
    return Status(StatusCode::TruncatedInput, "journal record payload is incomplete",
                  "declared=" + std::to_string(payload_length) +
                      " available=" + std::to_string(frame.size() - kJournalHeaderSize));
  }
  const ByteSpan payload = frame.subspan(kJournalHeaderSize, payload_length);
  if (crc32c(kPayloadCrcSeed, payload) != payload_crc) {
    return Status(StatusCode::IntegrityFailure, "journal record payload failed its CRC-32C check");
  }

  JournalEntry entry;
  entry.kind = *kind;
  entry.sequence = sequence;
  ByteReader reader(payload);
  if (!decode_payload(reader, entry, default_decode_limits())) {
    return reader.status().code() == StatusCode::Ok
               ? Status(StatusCode::CorruptionDetected, "journal record payload could not be decoded")
               : reader.status();
  }
  Status end = reader.expect_end("journal record");
  if (!end.ok()) return end;
  out = std::move(entry);
  return Status::success();
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------
std::vector<std::byte> encode_snapshot(const DurableImage& image) {
  ByteWriter payload;
  encode_image_payload(payload, image);

  const std::uint64_t payload_length = payload.size();
  const std::uint32_t payload_crc = crc32c(kPayloadCrcSeed, payload.span());

  std::vector<std::byte> out(kSnapshotHeaderSize + payload.size() + kSnapshotTrailerSize);
  const MutableByteSpan header(out.data(), kSnapshotHeaderSize);
  store_u32(header, 0, kSnapshotMagic);
  store_u16(header, 4, kSnapshotFormatVersion);
  store_u16(header, 6, static_cast<std::uint16_t>(persistence_schema()));
  store_u64(header, 8, payload_length);
  store_u32(header, 16, payload_crc);
  // The header CRC covers the preceding bytes with the CRC field itself zeroed,
  // so verification is reproducible from the file alone.
  store_u32(header, 20, 0);
  store_u64(header, 24, image.sequence);
  store_u32(header, 20, crc32c(ByteSpan(out.data(), kSnapshotHeaderSize - 4)));

  std::memcpy(out.data() + kSnapshotHeaderSize, payload.span().data(), payload.size());
  const auto digest = Sha256::digest(ByteSpan(out.data(), kSnapshotHeaderSize + payload.size()));
  std::memcpy(out.data() + kSnapshotHeaderSize + payload.size(), digest.data(), digest.size());
  return out;
}

Status decode_snapshot(ByteSpan bytes, const StoreLimits& limits, DurableImage& out) {
  if (bytes.size() < kSnapshotHeaderSize + kSnapshotTrailerSize) {
    return Status(StatusCode::TruncatedInput, "snapshot is shorter than its header and trailer");
  }
  if (load_u32(bytes, 0) != kSnapshotMagic) {
    return Status(StatusCode::CorruptionDetected, "snapshot magic mismatch");
  }
  const std::uint16_t format = load_u16(bytes, 4);
  if (format != kSnapshotFormatVersion) {
    return Status(StatusCode::UnsupportedSchema, "snapshot format version is not supported",
                  std::to_string(format));
  }
  const std::uint16_t schema = load_u16(bytes, 6);
  if (schema != static_cast<std::uint16_t>(persistence_schema())) {
    return Status(StatusCode::UnsupportedSchema, "snapshot schema is not supported",
                  std::to_string(schema));
  }
  const std::uint64_t payload_length = load_u64(bytes, 8);
  const std::uint32_t payload_crc = load_u32(bytes, 16);
  const std::uint32_t header_crc = load_u32(bytes, 20);

  const auto expected_total = checked_add(payload_length, kSnapshotHeaderSize + kSnapshotTrailerSize);
  if (!expected_total.has_value() || *expected_total > limits.max_snapshot_bytes) {
    return Status(StatusCode::OversizedInput, "snapshot payload length exceeds the configured bound",
                  "declared=" + std::to_string(payload_length));
  }
  if (bytes.size() != *expected_total) {
    return Status(StatusCode::TrailingGarbage,
                  "snapshot size does not match the declared payload length",
                  "declared=" + std::to_string(*expected_total) +
                      " actual=" + std::to_string(bytes.size()));
  }

  std::vector<std::byte> header_copy(bytes.begin(), bytes.begin() + kSnapshotHeaderSize);
  store_u32(MutableByteSpan(header_copy), 20, 0);
  if (crc32c(ByteSpan(header_copy.data(), kSnapshotHeaderSize - 4)) != header_crc) {
    return Status(StatusCode::IntegrityFailure, "snapshot header failed its CRC-32C check");
  }

  const ByteSpan payload = bytes.subspan(kSnapshotHeaderSize,
                                         static_cast<std::size_t>(payload_length));
  if (crc32c(kPayloadCrcSeed, payload) != payload_crc) {
    return Status(StatusCode::IntegrityFailure, "snapshot payload failed its CRC-32C check");
  }

  const ByteSpan trailer = bytes.subspan(kSnapshotHeaderSize + payload_length,
                                         kSnapshotTrailerSize);
  const auto digest = Sha256::digest(bytes.first(kSnapshotHeaderSize + payload_length));
  if (!digest_equal(trailer, ByteSpan(digest.data(), digest.size()))) {
    return Status(StatusCode::IntegrityFailure, "snapshot failed its SHA-256 integrity check");
  }

  DurableImage image;
  ByteReader reader(payload);
  if (!decode_image_payload(reader, image, default_decode_limits())) {
    return reader.status().code() == StatusCode::Ok
               ? Status(StatusCode::CorruptionDetected, "snapshot payload could not be decoded")
               : reader.status();
  }
  Status end = reader.expect_end("snapshot");
  if (!end.ok()) return end;

  const std::uint64_t declared_sequence = load_u64(bytes, 24);
  if (declared_sequence != image.sequence) {
    return Status(StatusCode::IntegrityFailure, "snapshot sequence disagrees with its payload");
  }
  if (image.record_count() > limits.max_records) {
    return Status(StatusCode::OversizedInput, "snapshot exceeds the configured record bound");
  }
  out = std::move(image);
  return Status::success();
}

// ---------------------------------------------------------------------------
// Image mutation
// ---------------------------------------------------------------------------
Status apply_journal_entry(DurableImage& image, const JournalEntry& entry, const StoreLimits&) {
  if (entry.sequence == 0) {
    return Status(StatusCode::InvalidArgument, "journal entry carries no log position");
  }
  switch (entry.kind) {
    case JournalEntryKind::Snapshot:
      return Status::success();
    case JournalEntryKind::UpsertDomain:
      image.domains[entry.domain.id] = entry.domain;
      break;
    case JournalEntryKind::UpsertParticipant:
      image.participants[entry.participant.id] = entry.participant;
      break;
    case JournalEntryKind::RemoveParticipant:
      image.participants.erase(entry.remove_participant);
      break;
    case JournalEntryKind::UpsertObject:
      image.objects[entry.object.id] = entry.object;
      break;
    case JournalEntryKind::RemoveObject:
      image.objects.erase(entry.remove_object);
      break;
    case JournalEntryKind::UpsertRegion:
      image.regions[entry.region.id] = entry.region;
      break;
    case JournalEntryKind::RemoveRegion:
      image.regions.erase(entry.remove_region);
      break;
    case JournalEntryKind::UpsertInvalidation:
      image.invalidations[entry.invalidation.id] = entry.invalidation;
      break;
    case JournalEntryKind::UpsertSync:
      image.syncs[entry.sync.id] = entry.sync;
      break;
    case JournalEntryKind::RemoveSync:
      image.syncs.erase(entry.remove_sync);
      break;
    case JournalEntryKind::UpsertPolicy:
      image.policies[entry.policy.id] = entry.policy;
      break;
    case JournalEntryKind::SetPendingPublication:
      image.pending_publications[entry.pending.id] = entry.pending;
      break;
    case JournalEntryKind::ClearPendingPublication: {
      for (auto it = image.pending_publications.begin(); it != image.pending_publications.end();) {
        if (it->second.object == entry.remove_object) {
          it = image.pending_publications.erase(it);
        } else {
          ++it;
        }
      }
      break;
    }
    case JournalEntryKind::SetEpoch:
      image.epoch = entry.epoch;
      break;
    case JournalEntryKind::Invalid:
      return Status(StatusCode::ProtocolViolation, "journal entry kind is not applicable");
  }
  if (entry.sequence > image.sequence) image.sequence = entry.sequence;
  return Status::success();
}

void scrub_object_for_persistence(ObjectRecord& record) {
  // Read grants and write authority are dynamic. They must never be restored as
  // if they were still valid, so they are removed before the record is written
  // rather than being written and then ignored.
  record.reads.clear();
  record.authority = AuthorityMode::None;
  record.writer = ParticipantId::nil();
  record.writer_boot = ParticipantBootId::nil();
  record.has_unpublished_dirty = false;
}

void scrub_participant_for_persistence(ParticipantRecord& record) {
  // A session and its replay counter belong to a live connection.
  record.session = SessionId::nil();
  record.live = false;
  record.last_sequence = OperationSequence::nil();
}

// ---------------------------------------------------------------------------
// DurableStore base
// ---------------------------------------------------------------------------
DurableStore::~DurableStore() = default;

Status DurableStore::append_batch(const std::vector<JournalEntry>& entries) {
  for (const JournalEntry& entry : entries) {
    Status status = append(entry);
    if (!status.ok()) return status;
  }
  return flush();
}
Status DurableStore::flush() { return Status::success(); }
Status DurableStore::close() { return Status::success(); }

// ---------------------------------------------------------------------------
// FileDurableStore
// ---------------------------------------------------------------------------
FileDurableStore::FileDurableStore(std::filesystem::path directory, StoreLimits limits)
    : directory_(std::move(directory)), limits_(limits) {}

FileDurableStore::~FileDurableStore() { (void)close(); }

std::filesystem::path FileDurableStore::snapshot_path() const {
  return directory_ / "coherence-fabric.snapshot";
}

std::filesystem::path FileDurableStore::journal_path() const {
  return directory_ / "coherence-fabric.journal";
}

Result<LoadReport> FileDurableStore::open(DurableImage& image) {
  LoadReport report;
  Status created = ensure_directory(directory_);
  if (!created.ok()) return Result<LoadReport>::failure(created);

  image = DurableImage{};

  if (path_exists(snapshot_path())) {
    const auto size = query_file_size(snapshot_path());
    report.snapshot_bytes = size.value_or(0);
    auto bytes = read_entire_file(snapshot_path(), limits_.max_snapshot_bytes);
    if (!bytes.has_value()) {
      return Result<LoadReport>::failure(bytes.status());
    }
    DurableImage loaded;
    Status decoded = decode_snapshot(ByteSpan(bytes.value().bytes), limits_, loaded);
    if (!decoded.ok()) {
      return Result<LoadReport>::failure(
          Status(decoded.code(), "durable snapshot rejected", decoded.to_string()));
    }
    image = std::move(loaded);
    report.snapshot_loaded = true;
  }

  report.journal_bytes = query_file_size(journal_path()).value_or(0);
  if (report.journal_bytes > limits_.max_journal_bytes) {
    return Result<LoadReport>::failure(
        Status(StatusCode::OversizedInput, "journal exceeds the configured bound",
               std::to_string(report.journal_bytes)));
  }

  if (path_exists(journal_path())) {
    auto bytes = read_entire_file(journal_path(), limits_.max_journal_bytes);
    if (!bytes.has_value()) return Result<LoadReport>::failure(bytes.status());
    const ByteSpan data(bytes.value().bytes);
    std::size_t offset = 0;
    while (offset < data.size()) {
      if (data.size() - offset < kJournalHeaderSize) {
        report.truncated_tail = true;
        ++report.corrupt_records;
        break;
      }
      const std::uint32_t payload_length = load_u32(data, offset + 16);
      if (payload_length > limits_.max_journal_record_bytes) {
        report.truncated_tail = true;
        ++report.corrupt_records;
        break;
      }
      const std::uint64_t frame_size = kJournalHeaderSize + payload_length;
      if (data.size() - offset < frame_size) {
        report.truncated_tail = true;
        ++report.corrupt_records;
        break;
      }
      JournalEntry entry;
      Status decoded = decode_journal_entry(data.subspan(offset, frame_size), limits_, entry);
      if (!decoded.ok()) {
        ++report.corrupt_records;
        report.truncated_tail = true;
        break;
      }
      Status applied = apply_journal_entry(image, entry, limits_);
      if (!applied.ok()) {
        ++report.corrupt_records;
        break;
      }
      ++report.journal_records_replayed;
      offset += static_cast<std::size_t>(frame_size);
    }
  }

  next_sequence_ = image.sequence + 1;
  auto handle = FileHandle::open_append(journal_path());
  if (!handle.has_value()) return Result<LoadReport>::failure(handle.status());
  journal_ = std::make_unique<FileHandle>(std::move(handle.value()));
  opened_ = true;
  return Result<LoadReport>::success(report);
}

Status FileDurableStore::append(const JournalEntry& entry) {
  if (!opened_ || journal_ == nullptr) {
    return Status(StatusCode::PersistenceFailure, "durable store is not open");
  }
  JournalEntry stamped = entry;
  if (stamped.sequence == 0) {
    stamped.sequence = next_sequence_++;
  } else if (stamped.sequence >= next_sequence_) {
    next_sequence_ = stamped.sequence + 1;
  }
  const std::vector<std::byte> frame = encode_journal_entry(stamped);
  if (frame.empty()) {
    return Status(StatusCode::InvalidArgument, "journal entry could not be encoded",
                  std::string(to_token(entry.kind)));
  }
  Status wrote = journal_->write(ByteSpan(frame));
  if (!wrote.ok()) return wrote;
  if (limits_.barrier_per_append) {
    Status flushed = journal_->flush();
    if (!flushed.ok()) return flushed;
  }
  bytes_since_compaction_ += frame.size();
  return Status::success();
}

Status FileDurableStore::compact(const DurableImage& image) {
  if (!opened_) {
    return Status(StatusCode::PersistenceFailure, "durable store is not open");
  }
  const std::vector<std::byte> bytes = encode_snapshot(image);
  // Close the journal before touching it so no handle outlives the replacement.
  Status closed = journal_->close();
  if (!closed.ok()) return closed;

  Status replaced = atomic_replace_file(snapshot_path(), ByteSpan(bytes));
  if (!replaced.ok()) {
    auto reopened = FileHandle::open_append(journal_path());
    if (reopened.has_value()) {
      journal_ = std::make_unique<FileHandle>(std::move(reopened.value()));
    }
    return replaced;
  }

  Status rotated = rotate_file(journal_path(), journal_path().string() + ".compacted");
  if (!rotated.ok()) return rotated;
  Status removed = remove_file(journal_path().string() + ".compacted");
  if (!removed.ok()) return removed;

  auto reopened = FileHandle::open_append(journal_path());
  if (!reopened.has_value()) return reopened.status();
  journal_ = std::make_unique<FileHandle>(std::move(reopened.value()));
  bytes_since_compaction_ = 0;
  next_sequence_ = image.sequence + 1;
  return Status::success();
}

Status FileDurableStore::flush() {
  if (!opened_ || journal_ == nullptr) return Status::success();
  return journal_->flush();
}

Status FileDurableStore::close() {
  if (journal_ != nullptr) {
    Status closed = journal_->close();
    journal_.reset();
    opened_ = false;
    return closed;
  }
  opened_ = false;
  return Status::success();
}

std::uint64_t FileDurableStore::journal_bytes() const {
  if (journal_ != nullptr) return journal_->size();
  return query_file_size(journal_path()).value_or(0);
}

std::string FileDurableStore::describe() const {
  return "file_durable_store directory=" + directory_.string();
}

// ---------------------------------------------------------------------------
// MemoryDurableStore
// ---------------------------------------------------------------------------
MemoryDurableStore::MemoryDurableStore(StoreLimits limits) : limits_(limits) {}
MemoryDurableStore::~MemoryDurableStore() = default;

Result<LoadReport> MemoryDurableStore::open(DurableImage& image) {
  LoadReport report;
  if (closed_) {
    return Result<LoadReport>::failure(
        Status(StatusCode::PersistenceFailure, "durable store is closed"));
  }
  image = DurableImage{};
  if (!snapshot_.empty()) {
    DurableImage loaded;
    Status decoded = decode_snapshot(ByteSpan(snapshot_), limits_, loaded);
    if (!decoded.ok()) {
      return Result<LoadReport>::failure(
          Status(decoded.code(), "durable snapshot rejected", decoded.to_string()));
    }
    image = std::move(loaded);
    report.snapshot_loaded = true;
    report.snapshot_bytes = snapshot_.size();
  }
  report.journal_bytes = journal_.size();
  const ByteSpan data(journal_);
  std::size_t offset = 0;
  std::uint64_t replayed = 0;
  while (offset < data.size()) {
    if (data.size() - offset < kJournalHeaderSize) {
      report.truncated_tail = true;
      ++report.corrupt_records;
      break;
    }
    const std::uint32_t payload_length = load_u32(data, offset + 16);
    if (payload_length > limits_.max_journal_record_bytes) {
      report.truncated_tail = true;
      ++report.corrupt_records;
      break;
    }
    const std::uint64_t frame_size = kJournalHeaderSize + payload_length;
    if (data.size() - offset < frame_size) {
      report.truncated_tail = true;
      ++report.corrupt_records;
      break;
    }
    JournalEntry entry;
    Status decoded = decode_journal_entry(data.subspan(offset, static_cast<std::size_t>(frame_size)),
                                          limits_, entry);
    if (!decoded.ok()) {
      ++report.corrupt_records;
      report.truncated_tail = true;
      break;
    }
    Status applied = apply_journal_entry(image, entry, limits_);
    if (!applied.ok()) {
      ++report.corrupt_records;
      break;
    }
    ++replayed;
    offset += static_cast<std::size_t>(frame_size);
  }
  report.journal_records_replayed = replayed;
  sequence_ = image.sequence;
  next_sequence_ = image.sequence + 1;
  opened_ = true;
  return Result<LoadReport>::success(report);
}

Status MemoryDurableStore::append(const JournalEntry& entry) {
  if (closed_ || !opened_) {
    return Status(StatusCode::PersistenceFailure, "durable store is not open");
  }
  ++append_count_;
  if (fail_append_at_ != 0 && append_count_ == fail_append_at_) {
    return Status(StatusCode::PersistenceFailure, "injected append failure",
                  std::to_string(append_count_));
  }
  JournalEntry stamped = entry;
  if (stamped.sequence == 0) {
    stamped.sequence = next_sequence_++;
  } else if (stamped.sequence >= next_sequence_) {
    next_sequence_ = stamped.sequence + 1;
  }
  const std::vector<std::byte> frame = encode_journal_entry(stamped);
  if (frame.empty()) {
    return Status(StatusCode::InvalidArgument, "journal entry could not be encoded");
  }
  journal_.insert(journal_.end(), frame.begin(), frame.end());
  sequence_ = stamped.sequence;
  return Status::success();
}

Status MemoryDurableStore::compact(const DurableImage& image) {
  if (closed_ || !opened_) {
    return Status(StatusCode::PersistenceFailure, "durable store is not open");
  }
  snapshot_ = encode_snapshot(image);
  journal_.clear();
  sequence_ = image.sequence;
  next_sequence_ = image.sequence + 1;
  return Status::success();
}

Status MemoryDurableStore::close() {
  closed_ = true;
  opened_ = false;
  return Status::success();
}

std::uint64_t MemoryDurableStore::journal_bytes() const { return journal_.size(); }

std::string MemoryDurableStore::describe() const { return "memory_durable_store"; }

} // namespace coherence
