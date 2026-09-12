// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Control-plane client.
#ifndef COHERENCE_CLIENT_HPP
#define COHERENCE_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/export.hpp"
#include "coherence/protocol.hpp"
#include "coherence/status.hpp"
#include "coherence/transport.hpp"

namespace coherence {

struct COHERENCE_API ClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string participant_name;
  ParticipantBootId boot;
  std::string node_label;
  bool observer = false;
  FrameLimits frames;
};

/// A request/response exchange with the coordinator.
struct COHERENCE_API Exchange {
  ResponseEnvelope envelope;
  std::vector<std::byte> payload;
};

class COHERENCE_API ControlClient {
 public:
  ControlClient() = default;
  ~ControlClient();
  ControlClient(const ControlClient&) = delete;
  ControlClient& operator=(const ControlClient&) = delete;

  /// Connect and complete the handshake. The coordinator replies with the
  /// session identity and its current epoch, which every later request must
  /// carry. A stale epoch is rejected by the coordinator, which is how
  /// pre-restart authority is fenced.
  Status connect_and_handshake(const ClientOptions& options);

  /// Send one request and wait for the response. Frame sequence numbers are
  /// strictly increasing per session, which makes a replayed frame detectable.
  Result<Exchange> request(MessageType type, ByteSpan body);

  /// Convenience helpers for the operations the CLI, agent and tests use.
  Result<Exchange> ping(std::uint64_t nonce);

  [[nodiscard]] bool connected() const noexcept { return socket_.valid(); }
  [[nodiscard]] const HelloResponse& session() const noexcept { return session_; }
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return session_.epoch; }
  [[nodiscard]] ParticipantId participant() const noexcept { return session_.participant; }
  [[nodiscard]] const ClientOptions& options() const noexcept { return options_; }

  Status close();

 private:
  Socket socket_;
  HelloResponse session_;
  ClientOptions options_;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t next_correlation_ = 1;
  FrameLimits frames_;
};

/// Access the response envelope. The control client decodes and validates it
/// while receiving the response and removes it from the payload, so the payload
/// holds only the operation-specific part.
[[nodiscard]] COHERENCE_API bool decode_exchange_envelope(const Exchange& exchange,
                                                          ResponseEnvelope& out);

} // namespace coherence

#endif // COHERENCE_CLIENT_HPP
