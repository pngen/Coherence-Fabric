// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/platform_file.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace coherence {
namespace {

Status system_error_status(StatusCode code, std::string message,
                           const std::filesystem::path& path) {
#if defined(_WIN32)
  const DWORD error = ::GetLastError();
  return Status(code, std::move(message), path.string() + " win32=" + std::to_string(error));
#else
  return Status(code, std::move(message), path.string() + " errno=" + std::to_string(errno));
#endif
}

std::filesystem::path unique_sibling(const std::filesystem::path& path) {
  // The suffix combines a process-wide counter with the process id. It exists
  // only to avoid collisions between concurrent writers of the same target; it
  // is never used as an identity anywhere in the coherence model.
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp = path;
#if defined(_WIN32)
  temp += ".tmp-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(n);
#else
  temp += ".tmp-" + std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(n);
#endif
  return temp;
}

constexpr std::size_t kMaxTransferChunk = 0x40000000u;  // 1 GiB

} // namespace

FileHandle::~FileHandle() { (void)close(); }

FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_), size_(other.size_) {
  other.handle_ = nullptr;
  other.size_ = 0;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_ = other.handle_;
    size_ = other.size_;
    other.handle_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void FileHandle::reset() noexcept {
  handle_ = nullptr;
  size_ = 0;
}

Result<FileHandle> FileHandle::open_append(const std::filesystem::path& path) {
  FileHandle handle;
#if defined(_WIN32)
  HANDLE native = ::CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA | GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
  if (native == INVALID_HANDLE_VALUE) {
    return Result<FileHandle>::failure(
        system_error_status(StatusCode::PersistenceFailure, "cannot open file for append", path));
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(native, &size) == 0) {
    ::CloseHandle(native);
    return Result<FileHandle>::failure(
        system_error_status(StatusCode::PersistenceFailure, "cannot size file", path));
  }
  handle.handle_ = native;
  handle.size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    return Result<FileHandle>::failure(
        system_error_status(StatusCode::PersistenceFailure, "cannot open file for append", path));
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Result<FileHandle>::failure(
        system_error_status(StatusCode::PersistenceFailure, "cannot size file", path));
  }
  handle.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  handle.size_ = static_cast<std::uint64_t>(st.st_size);
#endif
  return Result<FileHandle>::success(std::move(handle));
}

bool FileHandle::valid() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
  return handle_ != nullptr;
#endif
}

Status FileHandle::write(ByteSpan data) {
  if (!valid()) {
    return Status(StatusCode::PersistenceFailure, "write on a closed file handle");
  }
  const auto* cursor = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t remaining = data.size();
  while (remaining > 0) {
#if defined(_WIN32)
    const DWORD chunk =
        static_cast<DWORD>(remaining > kMaxTransferChunk ? kMaxTransferChunk : remaining);
    DWORD written = 0;
    if (::WriteFile(static_cast<HANDLE>(handle_), cursor, chunk, &written, nullptr) == 0) {
      return system_error_status(StatusCode::PersistenceFailure, "write failed",
                                 std::filesystem::path{});
    }
    if (written == 0) {
      return Status(StatusCode::PersistenceFailure, "write made no progress");
    }
#else
    const std::size_t chunk = remaining > kMaxTransferChunk ? kMaxTransferChunk : remaining;
    const ssize_t written = ::write(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)),
                                    cursor, chunk);
    if (written <= 0) {
      if (written < 0 && errno == EINTR) continue;
      return system_error_status(StatusCode::PersistenceFailure, "write failed",
                                 std::filesystem::path{});
    }
#endif
    cursor += static_cast<std::size_t>(written);
    remaining -= static_cast<std::size_t>(written);
    size_ += static_cast<std::uint64_t>(written);
  }
  return Status::success();
}

Status FileHandle::flush() {
  if (!valid()) {
    return Status(StatusCode::PersistenceFailure, "flush on a closed file handle");
  }
#if defined(_WIN32)
  if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return system_error_status(StatusCode::PersistenceFailure, "durability barrier failed",
                               std::filesystem::path{});
  }
#else
  if (::fsync(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_))) != 0) {
    return system_error_status(StatusCode::PersistenceFailure, "durability barrier failed",
                               std::filesystem::path{});
  }
#endif
  return Status::success();
}

Status FileHandle::close() {
  if (!valid()) {
    reset();
    return Status::success();
  }
#if defined(_WIN32)
  const BOOL ok = ::CloseHandle(static_cast<HANDLE>(handle_));
  reset();
  if (ok == 0) return Status(StatusCode::PersistenceFailure, "close failed");
#else
  const int rc = ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)));
  reset();
  if (rc != 0) return Status(StatusCode::PersistenceFailure, "close failed");
#endif
  return Status::success();
}

Result<FileReadResult> read_entire_file(const std::filesystem::path& path,
                                        std::uint64_t max_bytes) {
  FileReadResult out;
  // The size is taken from the open handle rather than from the directory
  // entry, because a file another handle is currently appending to may report a
  // stale directory size. That matters for reading durable state while it is
  // being written.
#if defined(_WIN32)
  {
    HANDLE probe = ::CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return Result<FileReadResult>::failure(Status(
            StatusCode::UnknownRequest, "file does not exist or is a directory", path.string()));
      }
      return Result<FileReadResult>::failure(
          system_error_status(StatusCode::PersistenceFailure, "cannot open file", path));
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(probe, &size) == 0) {
      ::CloseHandle(probe);
      return Result<FileReadResult>::failure(
          system_error_status(StatusCode::PersistenceFailure, "cannot size file", path));
    }
    ::CloseHandle(probe);
    if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
      out.total_size = static_cast<std::uint64_t>(size.QuadPart);
      out.truncated_to_limit = true;
      return Result<FileReadResult>::failure(
          Status(StatusCode::OversizedInput, "file exceeds the configured read bound",
                 path.string() + " size=" + std::to_string(size.QuadPart) +
                     " bound=" + std::to_string(max_bytes)));
    }
  }
#else
  {
    const int probe = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (probe < 0) {
      return Result<FileReadResult>::failure(
          system_error_status(StatusCode::PersistenceFailure, "cannot open file", path));
    }
    struct stat st {};
    if (::fstat(probe, &st) != 0) {
      ::close(probe);
      return Result<FileReadResult>::failure(
          system_error_status(StatusCode::PersistenceFailure, "cannot size file", path));
    }
    ::close(probe);
    if (S_ISDIR(st.st_mode)) {
      return Result<FileReadResult>::failure(Status(
          StatusCode::UnknownRequest, "file does not exist or is a directory", path.string()));
    }
    if (static_cast<std::uint64_t>(st.st_size) > max_bytes) {
      out.total_size = static_cast<std::uint64_t>(st.st_size);
      out.truncated_to_limit = true;
      return Result<FileReadResult>::failure(
          Status(StatusCode::OversizedInput, "file exceeds the configured read bound",
                 path.string() + " size=" + std::to_string(st.st_size) +
                     " bound=" + std::to_string(max_bytes)));
    }
  }
#endif
  const auto size = query_file_size(path);
  if (!size.has_value()) {
    return Result<FileReadResult>::failure(
        Status(StatusCode::UnknownRequest, "file does not exist or is a directory", path.string()));
  }
#if defined(_WIN32)
  HANDLE native = ::CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (native == INVALID_HANDLE_VALUE) {
    return Result<FileReadResult>::failure(
        system_error_status(StatusCode::PersistenceFailure, "cannot open file", path));
  }
  out.bytes.resize(static_cast<std::size_t>(*size));
  std::size_t offset = 0;
  while (offset < out.bytes.size()) {
    const std::size_t want = out.bytes.size() - offset;
    const DWORD chunk = static_cast<DWORD>(want > kMaxTransferChunk ? kMaxTransferChunk : want);
    DWORD read = 0;
    if (::ReadFile(native, out.bytes.data() + offset, chunk, &read, nullptr) == 0) {
      ::CloseHandle(native);
      return Result<FileReadResult>::failure(
          system_error_status(StatusCode::PersistenceFailure, "read failed", path));
    }
    if (read == 0) break;
    offset += read;
  }
  ::CloseHandle(native);
  out.bytes.resize(offset);
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return Result<FileReadResult>::failure(
        system_error_status(StatusCode::PersistenceFailure, "cannot open file", path));
  }
  out.bytes.resize(static_cast<std::size_t>(*size));
  std::size_t offset = 0;
  while (offset < out.bytes.size()) {
    const ssize_t read = ::read(fd, out.bytes.data() + offset, out.bytes.size() - offset);
    if (read < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Result<FileReadResult>::failure(
          system_error_status(StatusCode::PersistenceFailure, "read failed", path));
    }
    if (read == 0) break;
    offset += static_cast<std::size_t>(read);
  }
  ::close(fd);
  out.bytes.resize(offset);
#endif
  out.total_size = *size;
  return Result<FileReadResult>::success(std::move(out));
}

Status atomic_replace_file(const std::filesystem::path& path, ByteSpan data) {
  const std::filesystem::path temp = unique_sibling(path);
  {
    auto handle = FileHandle::open_append(temp);
    if (!handle.has_value()) return handle.status();
    Status wrote = handle.value().write(data);
    if (!wrote.ok()) {
      (void)handle.value().close();
      (void)remove_file(temp);
      return wrote;
    }
    Status flushed = handle.value().flush();
    if (!flushed.ok()) {
      (void)handle.value().close();
      (void)remove_file(temp);
      return flushed;
    }
    Status closed = handle.value().close();
    if (!closed.ok()) {
      (void)remove_file(temp);
      return closed;
    }
  }
#if defined(_WIN32)
  if (::MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    (void)remove_file(temp);
    return system_error_status(StatusCode::PersistenceFailure, "atomic replace failed", path);
  }
#else
  if (::rename(temp.c_str(), path.c_str()) != 0) {
    (void)remove_file(temp);
    return system_error_status(StatusCode::PersistenceFailure, "atomic replace failed", path);
  }
  const std::filesystem::path dir = path.parent_path();
  const int dir_fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (dir_fd >= 0) {
    (void)::fsync(dir_fd);
    ::close(dir_fd);
  }
#endif
  return Status::success();
}

Status ensure_directory(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    if (!std::filesystem::is_directory(path, ec)) {
      return Status(StatusCode::PersistenceFailure, "path exists and is not a directory",
                    path.string());
    }
    return Status::success();
  }
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return Status(StatusCode::PersistenceFailure, "cannot create directory",
                  path.string() + ": " + ec.message());
  }
  return Status::success();
}

bool path_exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

std::optional<std::uint64_t> query_file_size(const std::filesystem::path& path) {
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (::GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &info) == 0) {
    return std::nullopt;
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return std::nullopt;
  const std::uint64_t size =
      (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  return size;
#else
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
  if (S_ISDIR(st.st_mode)) return std::nullopt;
  return static_cast<std::uint64_t>(st.st_size);
#endif
}

Status remove_file(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return Status(StatusCode::PersistenceFailure, "cannot remove file",
                  path.string() + ": " + ec.message());
  }
  return Status::success();
}

Status rotate_file(const std::filesystem::path& path, const std::filesystem::path& backup) {
  if (!path_exists(path)) return Status::success();
  std::error_code ec;
  std::filesystem::remove(backup, ec);
  ec.clear();
  std::filesystem::rename(path, backup, ec);
  if (ec) {
    return Status(StatusCode::PersistenceFailure, "cannot rotate file",
                  path.string() + " -> " + backup.string() + ": " + ec.message());
  }
  return Status::success();
}

} // namespace coherence
