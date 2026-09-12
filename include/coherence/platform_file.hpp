// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Minimal, portable file primitives with explicit durability semantics.
#ifndef COHERENCE_PLATFORM_FILE_HPP
#define COHERENCE_PLATFORM_FILE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/export.hpp"
#include "coherence/status.hpp"

namespace coherence {

class COHERENCE_API FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle();
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;

  /// Open (creating if necessary) for append. Fails if the path is a directory.
  [[nodiscard]] static Result<FileHandle> open_append(const std::filesystem::path& path);

  [[nodiscard]] bool valid() const noexcept;
  Status write(ByteSpan data);
  /// Flush user buffers and issue the platform durability barrier.
  Status flush();
  Status close();
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

 private:
  void reset() noexcept;
  void* handle_ = nullptr;
  std::uint64_t size_ = 0;
};

struct COHERENCE_API FileReadResult {
  std::vector<std::byte> bytes;
  std::uint64_t total_size = 0;
  bool truncated_to_limit = false;
};

/// Read an entire file. Fails when the file exceeds max_bytes so that a hostile
/// or corrupt file cannot cause an unbounded allocation.
[[nodiscard]] COHERENCE_API Result<FileReadResult> read_entire_file(
    const std::filesystem::path& path, std::uint64_t max_bytes);

/// Write bytes to a temporary sibling file, flush it, then atomically replace
/// the target. The temporary file is removed on failure.
[[nodiscard]] COHERENCE_API Status atomic_replace_file(const std::filesystem::path& path,
                                                       ByteSpan data);

[[nodiscard]] COHERENCE_API Status ensure_directory(const std::filesystem::path& path);
[[nodiscard]] COHERENCE_API bool path_exists(const std::filesystem::path& path);
[[nodiscard]] COHERENCE_API std::optional<std::uint64_t> query_file_size(
    const std::filesystem::path& path);
[[nodiscard]] COHERENCE_API Status remove_file(const std::filesystem::path& path);

/// Rename a file onto a backup name, replacing any previous backup. Used by the
/// journal before compaction so that a crash during compaction is recoverable.
[[nodiscard]] COHERENCE_API Status rotate_file(const std::filesystem::path& path,
                                               const std::filesystem::path& backup);

} // namespace coherence

#endif // COHERENCE_PLATFORM_FILE_HPP
