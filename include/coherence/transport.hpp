// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Framed TCP transport used by the distributed control plane.
//
// This is an ordinary stream transport. It is NOT RDMA, NOT a remote direct
// memory path, and NOT a coherent interconnect. Coherence Fabric uses it to
// carry control messages; any byte movement described as synchronization is
// either performed inside one process or through an explicitly named shared
// mapping, never through this socket.
#ifndef COHERENCE_TRANSPORT_HPP
#define COHERENCE_TRANSPORT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "coherence/bytes.hpp"
#include "coherence/export.hpp"
#include "coherence/status.hpp"

namespace coherence {

/// Initialise the platform networking stack. Idempotent and thread-safe.
COHERENCE_API void initialize_networking();

class COHERENCE_API Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] static Result<Socket> connect(const std::string& host, std::uint16_t port);
  [[nodiscard]] static Result<Socket> adopt(std::uintptr_t native);

  [[nodiscard]] bool valid() const noexcept;
  Status send_all(ByteSpan data);
  Status recv_exact(MutableByteSpan out);
  Status set_no_delay(bool enabled);
  /// Shut the socket down in both directions. Safe to call from another thread
  /// while a blocking read or write is in progress: it makes those calls fail
  /// rather than leaving them blocked forever.
  Status shutdown();
  Status close();
  [[nodiscard]] std::string peer() const;
  [[nodiscard]] std::uintptr_t native() const noexcept { return handle_; }

 private:
  void reset() noexcept;
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
  std::string peer_;
};

class COHERENCE_API Listener {
 public:
  Listener() = default;
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;

  /// Bind and listen. A port of 0 selects an ephemeral port, reported by port().
  [[nodiscard]] static Result<Listener> bind(const std::string& host, std::uint16_t port,
                                             int backlog = 64);
  [[nodiscard]] Result<Socket> accept();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  /// Make a blocking accept() return a failure.
  Status interrupt();
  Status close();

 private:
  void reset() noexcept;
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
  std::uint16_t port_ = 0;
};

/// Resolve "host:port" text. Supports "host", "host:port" and "[v6]:port".
[[nodiscard]] COHERENCE_API Result<std::pair<std::string, std::uint16_t>> parse_endpoint(
    std::string_view text, std::uint16_t default_port);

} // namespace coherence

#endif // COHERENCE_TRANSPORT_HPP
