// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// The coordinator process: owner of distributed coherence authority.
#ifndef COHERENCE_COORDINATOR_HPP
#define COHERENCE_COORDINATOR_HPP

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/export.hpp"
#include "coherence/protocol.hpp"
#include "coherence/status.hpp"
#include "coherence/transport.hpp"

namespace coherence {

struct COHERENCE_API CoordinatorOptions {
  std::string bind_host = "127.0.0.1";
  std::uint16_t port = 0;
  /// Empty means ephemeral: no durable state is written and publications are
  /// reported as ephemeral rather than durable.
  std::filesystem::path state_directory;
  std::string domain_name = "default";
  std::string node_label;
  EngineConfig engine;
  FrameLimits frames;
  std::uint64_t max_sessions = 256;
  /// Bound on retained correlation identities per session, used for duplicate
  /// and replay detection.
  std::uint64_t correlation_window = 4096;
};

class COHERENCE_API Coordinator {
 public:
  explicit Coordinator(CoordinatorOptions options);
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  /// Create the domain, attach durable state (recovering if present) and bind
  /// the listener. Must be called before run() or accept_and_serve_one().
  Status start();

  /// Accept and serve connections until stop() is called.
  Status run();

  /// Accept and serve exactly one connection in the calling thread. Used by
  /// tests that need deterministic ordering.
  Status accept_and_serve_one();

  /// Stop serving. Idempotent. Revokes further admission, shuts every session
  /// socket down, joins every connection thread and closes the listener.
  Status stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool running() const noexcept { return started_; }
  [[nodiscard]] CoherenceEngine& engine() noexcept { return *engine_; }
  [[nodiscard]] const CoherenceEngine& engine() const noexcept { return *engine_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] CoherenceDomainId domain() const noexcept { return domain_; }
  [[nodiscard]] const CoordinatorOptions& options() const noexcept { return options_; }
  [[nodiscard]] std::uint64_t active_sessions() const;

  /// Deterministic single-block status rendering used by the CLI and tests.
  [[nodiscard]] std::string render_status() const;

  /// Shut the coordinator down through the engine after revoking authority.
  Status shutdown_engine();

 private:
  struct Session;
  struct Impl;

  Status serve_socket(Socket socket);
  Status dispatch(const std::shared_ptr<Session>& session, const Frame& frame, ByteWriter& body,
                  ResponseEnvelope& envelope);

  CoordinatorOptions options_;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<CoherenceEngine> engine_;
  Listener listener_;
  std::uint16_t port_ = 0;
  bool started_ = false;
  CoherenceDomainId domain_;
  RecoveryReport recovery_;
};

} // namespace coherence

#endif // COHERENCE_COORDINATOR_HPP
