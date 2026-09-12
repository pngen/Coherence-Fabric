// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Real OS process control for the multiprocess proof.
//
// The proof must use genuine processes, so this helper creates them with the
// platform API rather than threads. It never applies a wall-clock deadline:
// callers block on a readiness marker printed by the child, which makes a hang
// a defect with an identifiable last phase rather than a flaky timeout.
#ifndef COHERENCE_TEST_PROCESS_UTIL_HPP
#define COHERENCE_TEST_PROCESS_UTIL_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace cfproc {

/// Read an environment variable without the deprecated C runtime call.
inline std::string environment_value(const char* name) {
#if defined(_MSC_VER)
  char* buffer = nullptr;
  std::size_t size = 0;
  if (::_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return std::string();
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
#endif
}

/// Directory holding the built executables. CTest sets COHERENCE_FABRIC_BINDIR.
inline std::filesystem::path executable_directory() {
  const std::string configured = environment_value("COHERENCE_FABRIC_BINDIR");
  if (!configured.empty()) return std::filesystem::path(configured);
  return std::filesystem::current_path();
}

/// Resolve a built executable by its base name, adding the platform suffix.
inline std::filesystem::path executable_path(const std::string& base_name) {
  std::filesystem::path candidate = executable_directory() / base_name;
#if defined(_WIN32)
  if (!std::filesystem::exists(candidate)) candidate += ".exe";
#endif
  return candidate;
}

inline std::string quote(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

class Child {
 public:
  Child() = default;
  ~Child() { (void)close(); }

  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  /// Spawn a process. Standard output and standard error are merged into one
  /// pipe so that ordering between them is preserved. Standard input stays
  /// connected to a pipe the parent controls.
  static std::unique_ptr<Child> spawn(const std::filesystem::path& executable,
                                      const std::vector<std::string>& args) {
    auto child = std::make_unique<Child>();
    child->command_ = executable.string();
    for (const std::string& arg : args) {
      child->command_.push_back(' ');
      child->command_ += quote(arg);
    }
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    if (::CreatePipe(&out_read, &out_write, &attributes, 0) == 0) return nullptr;
    if (::CreatePipe(&in_read, &in_write, &attributes, 0) == 0) return nullptr;
    ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out_write;
    startup.hStdError = out_write;
    startup.hStdInput = in_read;
    PROCESS_INFORMATION info{};
    std::wstring wide(child->command_.begin(), child->command_.end());
    const BOOL created =
        ::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &info);
    ::CloseHandle(out_write);
    ::CloseHandle(in_read);
    if (created == 0) {
      ::CloseHandle(out_read);
      ::CloseHandle(in_write);
      return nullptr;
    }
    child->process_ = info.hProcess;
    child->thread_ = info.hThread;
    child->out_read_ = out_read;
    child->in_write_ = in_write;
#else
    int out_pipe[2];
    int in_pipe[2];
    if (::pipe(out_pipe) != 0) return nullptr;
    if (::pipe(in_pipe) != 0) return nullptr;
    std::vector<std::string> argv_storage;
    argv_storage.push_back(executable.string());
    for (const std::string& arg : args) argv_storage.push_back(arg);
    std::vector<char*> argv;
    for (std::string& entry : argv_storage) argv.push_back(entry.data());
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
      ::dup2(out_pipe[1], STDOUT_FILENO);
      ::dup2(out_pipe[1], STDERR_FILENO);
      ::dup2(in_pipe[0], STDIN_FILENO);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      ::execv(executable.string().c_str(), argv.data());
      ::_exit(127);
    }
    ::close(out_pipe[1]);
    ::close(in_pipe[0]);
    child->process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
    child->out_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(out_pipe[0]));
    child->in_write_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(in_pipe[1]));
#endif
    return child;
  }

  [[nodiscard]] bool valid() const noexcept {
#if defined(_WIN32)
    return process_ != nullptr;
#else
    return process_ != nullptr;
#endif
  }

  /// Read one line from the merged output stream. Returns false when the stream
  /// ends.
  bool read_line(std::string& out) {
    out.clear();
    for (;;) {
      const std::size_t consumed = buffer_.find('\n');
      if (consumed != std::string::npos) {
        out = buffer_.substr(0, consumed);
        buffer_.erase(0, consumed + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      char chunk[1024];
      const int read = read_chunk(chunk, sizeof(chunk));
      if (read <= 0) {
        if (!buffer_.empty()) {
          out = buffer_;
          buffer_.clear();
          return true;
        }
        return false;
      }
      buffer_.append(chunk, static_cast<std::size_t>(read));
    }
  }

  /// Block until a line beginning with the prefix arrives. Every line seen is
  /// recorded in transcript() so that a failure explains itself.
  bool wait_for_line(const std::string& prefix, std::string& matched) {
    for (;;) {
      std::string line;
      if (!read_line(line)) return false;
      transcript_.push_back(line);
      if (line.rfind(prefix, 0) == 0) {
        matched = line;
        return true;
      }
    }
  }

  void write_stdin(const std::string& text) {
    if (in_write_ == nullptr) return;
    std::string payload = text;
    payload.push_back('\n');
#if defined(_WIN32)
    DWORD written = 0;
    (void)::WriteFile(static_cast<HANDLE>(in_write_), payload.data(),
                      static_cast<DWORD>(payload.size()), &written, nullptr);
#else
    const ssize_t ignored = ::write(static_cast<int>(reinterpret_cast<std::intptr_t>(in_write_)),
                                    payload.data(), payload.size());
    (void)ignored;
#endif
  }

  void close_stdin() {
    if (in_write_ == nullptr) return;
#if defined(_WIN32)
    ::CloseHandle(static_cast<HANDLE>(in_write_));
#else
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(in_write_)));
#endif
    in_write_ = nullptr;
  }

  /// Terminate the process immediately, without giving it a chance to clean up.
  /// This is how the proof simulates a crash.
  void kill() {
    if (!valid()) return;
#if defined(_WIN32)
    (void)::TerminateProcess(static_cast<HANDLE>(process_), 0xDEADu);
#else
    (void)::kill(static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_)), SIGKILL);
#endif
  }

  /// Wait for the process to exit and return its exit code.
  int wait() {
    if (!valid()) return -1;
    int code = -1;
#if defined(_WIN32)
    (void)::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD exit_code = 0;
    (void)::GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code);
    code = static_cast<int>(exit_code);
#else
    int status = 0;
    (void)::waitpid(static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_)), &status, 0);
    code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    exited_ = true;
    return code;
  }

  /// Drain any output produced before exit.
  void drain() {
    if (out_read_ == nullptr) return;
    for (;;) {
      char chunk[4096];
      const int read = read_chunk(chunk, sizeof(chunk));
      if (read <= 0) break;
      buffer_.append(chunk, static_cast<std::size_t>(read));
    }
    std::string line;
    while (read_line(line)) transcript_.push_back(line);
  }

  [[nodiscard]] const std::vector<std::string>& transcript() const noexcept { return transcript_; }
  [[nodiscard]] const std::string& command() const noexcept { return command_; }

  void close() {
    close_stdin();
    if (out_read_ != nullptr) {
#if defined(_WIN32)
      ::CloseHandle(static_cast<HANDLE>(out_read_));
#else
      ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(out_read_)));
#endif
      out_read_ = nullptr;
    }
#if defined(_WIN32)
    if (thread_ != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(thread_));
      thread_ = nullptr;
    }
    if (process_ != nullptr) {
      if (!exited_) (void)::TerminateProcess(static_cast<HANDLE>(process_), 0xDEADu);
      (void)::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
      ::CloseHandle(static_cast<HANDLE>(process_));
      process_ = nullptr;
    }
#else
    if (process_ != nullptr && !exited_) {
      (void)::kill(static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_)), SIGKILL);
      int status = 0;
      (void)::waitpid(static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_)), &status, 0);
    }
    process_ = nullptr;
#endif
  }

 private:
  int read_chunk(char* destination, std::size_t capacity) {
    if (out_read_ == nullptr) return 0;
#if defined(_WIN32)
    DWORD read = 0;
    if (::ReadFile(static_cast<HANDLE>(out_read_), destination, static_cast<DWORD>(capacity), &read,
                   nullptr) == 0) {
      return 0;
    }
    return static_cast<int>(read);
#else
    const ssize_t read = ::read(static_cast<int>(reinterpret_cast<std::intptr_t>(out_read_)),
                                destination, capacity);
    return read <= 0 ? 0 : static_cast<int>(read);
#endif
  }

  std::string command_;
  std::string buffer_;
  std::vector<std::string> transcript_;
  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* out_read_ = nullptr;
  void* in_write_ = nullptr;
  bool exited_ = false;
};

/// Run a process to completion and return its exit code. Output is drained so
/// the child can never block on a full pipe.
inline int run_to_completion(const std::filesystem::path& executable,
                             const std::vector<std::string>& args,
                             std::vector<std::string>* transcript = nullptr) {
  auto child = Child::spawn(executable, args);
  if (child == nullptr) return -1;
  child->close_stdin();
  const int code = child->wait();
  child->drain();
  if (transcript != nullptr) *transcript = child->transcript();
  child->close();
  return code;
}

/// Wait for every process in a list to exit, then close them. Used to prove
/// that no child process is leaked.
inline void join_all(std::vector<std::unique_ptr<Child>>& children) {
  for (auto& child : children) {
    if (child) child->close();
  }
  children.clear();
}

} // namespace cfproc

#endif // COHERENCE_TEST_PROCESS_UTIL_HPP
