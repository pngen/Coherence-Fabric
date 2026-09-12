// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Durable coherence metadata.
//
// Layout of a durable directory:
//   coherence-fabric.snapshot   full durable image, atomically replaced
//   coherence-fabric.journal    length-framed append-only records
//
// Authority durations:
//   * Authority granted before a restart is never valid after the restart; the
//     coordinator epoch always advances and every pre-restart boot is fenced.
//   * A region's dynamic currentness is never restored from durable metadata.
//     Restored regions are downgraded to RevalidationRequired and receive
//     PersistedMetadata evidence, which the evidence model refuses to treat as
//     proof of currentness.
//   * A publication becomes authoritative only after its pending record has
//     been appended to the journal and the durability barrier has returned
//     success. That is the exact durability point.
#ifndef COHERENCE_PERSISTENCE_HPP
#define COHERENCE_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/export.hpp"
#include "coherence/engine.hpp"
#include "coherence/model.hpp"
#include "coherence/platform_file.hpp"
#include "coherence/policy.hpp"
#include "coherence/serialize.hpp"
#include "coherence/status.hpp"

namespace coherence {

using DurableObjectMap = std::map<ObjectId, ObjectRecord>;
using DurableRegionMap = std::map<RegionId, RegionRecord>;
using DurableParticipantMap = std::map<ParticipantId, ParticipantRecord>;
using DurableDomainMap = std::map<CoherenceDomainId, DomainRecord>;
using DurableInvalidationMap = std::map<InvalidationId, InvalidationRecord>;
using DurableSyncMap = std::map<SyncOperationId, SyncRecord>;
using DurablePolicyMap = std::map<PolicyId, CoherencePolicy>;
using DurablePendingMap = std::map<PublicationId, PendingPublication>;

/// The complete durable image. Dynamic authority (read grants, writer
/// assignments, live sessions) is deliberately absent: none of it survives a
/// restart.
struct COHERENCE_API DurableImage {
  std::uint64_t sequence = 0;
  CoordinatorEpoch epoch;
  DurableDomainMap domains;
  DurableParticipantMap participants;
  DurableObjectMap objects;
  DurableRegionMap regions;
  DurableInvalidationMap invalidations;
  DurableSyncMap syncs;
  DurablePolicyMap policies;
  DurablePendingMap pending_publications;

  [[nodiscard]] std::uint64_t record_count() const noexcept {
    return static_cast<std::uint64_t>(domains.size() + participants.size() + objects.size() +
                                      regions.size() + invalidations.size() + syncs.size() +
                                      policies.size() + pending_publications.size());
  }
};

enum class JournalEntryKind : std::uint16_t {
  Invalid = 0,
  Snapshot = 1,
  UpsertDomain = 2,
  UpsertParticipant = 3,
  UpsertObject = 4,
  UpsertRegion = 5,
  UpsertInvalidation = 6,
  UpsertSync = 7,
  UpsertPolicy = 8,
  SetPendingPublication = 9,
  ClearPendingPublication = 10,
  RemoveObject = 11,
  RemoveRegion = 12,
  SetEpoch = 13,
  RemoveSync = 14,
  RemoveParticipant = 15,
};

COHERENCE_API std::string_view to_token(JournalEntryKind value) noexcept;
COHERENCE_API std::optional<JournalEntryKind> journal_kind_from_u16(std::uint16_t raw) noexcept;

struct COHERENCE_API JournalEntry {
  JournalEntryKind kind = JournalEntryKind::Invalid;
  std::uint64_t sequence = 0;
  // At most one payload is meaningful for a given kind.
  DomainRecord domain;
  ParticipantRecord participant;
  ObjectRecord object;
  RegionRecord region;
  InvalidationRecord invalidation;
  SyncRecord sync;
  CoherencePolicy policy;
  PendingPublication pending;
  CoherenceDomainId remove_object_domain;
  ObjectId remove_object;
  RegionId remove_region;
  InvalidationId remove_invalidation;
  SyncOperationId remove_sync;
  ParticipantId remove_participant;
  CoordinatorEpoch epoch;

  [[nodiscard]] std::string describe() const;
};

struct COHERENCE_API StoreLimits {
  /// When true every append issues a durability barrier before returning. When
  /// false the caller takes responsibility for calling flush() before relying
  /// on the data, which is what bulk loaders do. Turning this off never weakens
  /// the ordering guarantee: an entry is always written before the state it
  /// describes becomes observable.
  bool barrier_per_append = true;

  /// Largest snapshot file the store will read. Bounds attacker-influenced or
  /// accidental allocations.
  std::uint64_t max_snapshot_bytes = 1ull << 31;  // 2 GiB
  std::uint64_t max_journal_bytes = 1ull << 31;
  std::uint64_t max_journal_record_bytes = 1ull << 26;  // 64 MiB
  std::uint64_t max_records = 20000000;
};

struct COHERENCE_API LoadReport {
  std::uint64_t journal_records_replayed = 0;
  /// Records that failed framing or integrity checks. Replay stops at the
  /// first invalid record; anything after it is not trusted, because the
  /// journal is a strictly ordered log.
  std::uint64_t corrupt_records = 0;
  bool snapshot_loaded = false;
  std::uint64_t snapshot_bytes = 0;
  std::uint64_t journal_bytes = 0;
  bool truncated_tail = false;
};

class COHERENCE_API DurableStore {
 public:
  virtual ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  /// Create or open the durable location and load the current image.
  virtual Result<LoadReport> open(DurableImage& image) = 0;
  /// Append an entry and make it durable before returning success.
  virtual Status append(const JournalEntry& entry) = 0;
  /// Append several entries with a single durability barrier. The default
  /// implementation is correct but not batched.
  virtual Status append_batch(const std::vector<JournalEntry>& entries);
  /// Atomically replace the snapshot and truncate the journal.
  virtual Status compact(const DurableImage& image) = 0;
  virtual Status flush();
  virtual Status close();
  [[nodiscard]] virtual std::uint64_t journal_bytes() const = 0;
  [[nodiscard]] virtual std::string describe() const = 0;
  [[nodiscard]] virtual const StoreLimits& limits() const noexcept = 0;

 protected:
  DurableStore() = default;
};

/// Durable store backed by two files in a directory.
class COHERENCE_API FileDurableStore final : public DurableStore {
 public:
  explicit FileDurableStore(std::filesystem::path directory, StoreLimits limits = StoreLimits{});
  ~FileDurableStore() override;

  Result<LoadReport> open(DurableImage& image) override;
  Status append(const JournalEntry& entry) override;
  Status compact(const DurableImage& image) override;
  Status flush() override;
  Status close() override;
  [[nodiscard]] std::uint64_t journal_bytes() const override;
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] const StoreLimits& limits() const noexcept override { return limits_; }

  [[nodiscard]] std::filesystem::path snapshot_path() const;
  [[nodiscard]] std::filesystem::path journal_path() const;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  /// Bytes appended since the last compaction.
  [[nodiscard]] std::uint64_t bytes_since_compaction() const noexcept {
    return bytes_since_compaction_;
  }

 private:
  std::filesystem::path directory_;
  StoreLimits limits_;
  std::unique_ptr<FileHandle> journal_;
  std::uint64_t bytes_since_compaction_ = 0;
  std::uint64_t next_sequence_ = 1;
  bool opened_ = false;
};

/// In-memory store used by tests and by ephemeral deployments. It applies the
/// same journal-then-apply ordering as the file store, including the
/// configurable failure injection used by adversarial tests.
class COHERENCE_API MemoryDurableStore final : public DurableStore {
 public:
  explicit MemoryDurableStore(StoreLimits limits = StoreLimits{});
  ~MemoryDurableStore() override;

  Result<LoadReport> open(DurableImage& image) override;
  Status append(const JournalEntry& entry) override;
  Status compact(const DurableImage& image) override;
  Status close() override;
  [[nodiscard]] std::uint64_t journal_bytes() const override;
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] const StoreLimits& limits() const noexcept override { return limits_; }

  /// Fail the Nth append operation (1-based). Zero disables the injection.
  void fail_append_at(std::uint64_t ordinal) noexcept { fail_append_at_ = ordinal; }
  void set_closed(bool closed) noexcept { closed_ = closed; }
  [[nodiscard]] std::uint64_t append_count() const noexcept { return append_count_; }

  /// Direct access for tests that need to simulate a crash: the persisted
  /// journal and snapshot are exposed as byte buffers.
  [[nodiscard]] const std::vector<std::byte>& journal_bytes_raw() const noexcept {
    return journal_;
  }
  [[nodiscard]] const std::vector<std::byte>& snapshot_bytes_raw() const noexcept {
    return snapshot_;
  }
  void set_journal_bytes(std::vector<std::byte> bytes) { journal_ = std::move(bytes); }
  void set_snapshot_bytes(std::vector<std::byte> bytes) { snapshot_ = std::move(bytes); }
  [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }

 private:
  StoreLimits limits_;
  std::vector<std::byte> snapshot_;
  std::vector<std::byte> journal_;
  std::uint64_t sequence_ = 0;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t append_count_ = 0;
  std::uint64_t fail_append_at_ = 0;
  bool closed_ = false;
  bool opened_ = false;
};

// ---------------------------------------------------------------------------
// Journal record framing.
//
//   u32 magic
//   u16 format version
//   u16 entry kind
//   u64 sequence
//   u32 payload length
//   u32 payload crc32c
//   payload bytes
//
// The frame header is 24 bytes. A reader validates the magic, the version, the
// kind, the payload length against both the remaining input and the configured
// bound, and the CRC before applying anything.
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kJournalRecordMagic = 0x43464A31u;  // "CFJ1"
inline constexpr std::uint16_t kJournalFormatVersion = 1;
inline constexpr std::size_t kJournalHeaderSize = 24;

COHERENCE_API std::vector<std::byte> encode_journal_entry(const JournalEntry& entry);
COHERENCE_API Status decode_journal_entry(ByteSpan frame, const StoreLimits& limits,
                                          JournalEntry& out);

// ---------------------------------------------------------------------------
// Snapshot file format.
//
//   u32 magic "CFS1"
//   u16 format version
//   u16 schema
//   u64 sequence
//   u64 payload length
//   u32 payload crc32c
//   u32 header crc32c (over the preceding 32 bytes)
//   payload
//   sha256 of (header || payload)   -- 32 bytes
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kSnapshotMagic = 0x43465331u;  // "CFS1"
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;
inline constexpr std::size_t kSnapshotHeaderSize = 32;
inline constexpr std::size_t kSnapshotTrailerSize = 32;

COHERENCE_API std::vector<std::byte> encode_snapshot(const DurableImage& image);
COHERENCE_API Status decode_snapshot(ByteSpan bytes, const StoreLimits& limits, DurableImage& out);

// ---------------------------------------------------------------------------
// Image mutation helpers shared by the store and the engine.
// ---------------------------------------------------------------------------
COHERENCE_API Status apply_journal_entry(DurableImage& image, const JournalEntry& entry,
                                         const StoreLimits& limits);

/// Scrub state that must never be treated as durable authority. Called before
/// an object record is persisted.
COHERENCE_API void scrub_object_for_persistence(ObjectRecord& record);
COHERENCE_API void scrub_participant_for_persistence(ParticipantRecord& record);

} // namespace coherence

#endif // COHERENCE_PERSISTENCE_HPP
