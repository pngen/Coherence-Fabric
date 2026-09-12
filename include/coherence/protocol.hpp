// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Framed control-plane protocol.
//
// Frame layout (60-byte header, then the body):
//   offset  0  u32 magic ("CFG1")
//   offset  4  u16 protocol version
//   offset  6  u16 message type
//   offset  8  u32 flags
//   offset 12  u64 session identity (high)
//   offset 20  u64 session identity (low)
//   offset 28  u64 coordinator epoch the sender believes is current
//   offset 36  u64 per-session monotonic sequence
//   offset 44  u64 correlation identity
//   offset 52  u32 body length
//   offset 56  u32 body CRC-32C
//
// Every field is validated before anything is allocated or acted upon. In
// particular the body length is checked against the configured bound and the
// CRC is verified before the body is decoded.
#ifndef COHERENCE_PROTOCOL_HPP
#define COHERENCE_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/bytes.hpp"
#include "coherence/codec.hpp"
#include "coherence/engine.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/model.hpp"
#include "coherence/serialize.hpp"
#include "coherence/status.hpp"
#include "coherence/transport.hpp"

namespace coherence {

inline constexpr std::uint32_t kFrameMagic = 0x43464731u;  // "CFG1"
inline constexpr std::size_t kFrameHeaderSize = 60;

enum class MessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  Response = 3,
  Goodbye = 4,

  CreateDomain = 10,
  RegisterParticipant = 11,
  RegisterObject = 12,
  RegisterRegion = 13,
  AcquireRead = 14,
  AcquireWrite = 15,
  MarkDirty = 16,
  Publish = 17,
  Invalidate = 18,
  AcknowledgeInvalidation = 19,
  SyncBegin = 20,
  SyncComplete = 21,
  SyncFail = 22,
  Release = 23,
  Revalidate = 24,
  Query = 25,
  Snapshot = 26,
  Audit = 27,
  FenceParticipant = 28,
  RetireRegion = 29,
  RetireObject = 30,
  Shutdown = 31,
  Ping = 32,
  SetPolicy = 33,
  ListParticipants = 34,
  ListObjects = 35,
  ListRegions = 36,
  DecisionLog = 37,
  RecoveryInfo = 38,
  DrainSessions = 39,
  ListInvalidations = 40,
  ResolveRecovery = 41,
};

COHERENCE_API std::string_view to_token(MessageType value) noexcept;
COHERENCE_API std::optional<MessageType> message_type_from_u16(std::uint16_t raw) noexcept;

struct COHERENCE_API FrameLimits {
  std::uint32_t max_body_bytes = 16u * 1024u * 1024u;
  std::uint64_t max_blob_bytes = 16u * 1024u * 1024u;
};

struct COHERENCE_API FrameHeader {
  std::uint16_t version = 0;
  MessageType type = MessageType::Invalid;
  std::uint32_t flags = 0;
  SessionId session;
  CoordinatorEpoch epoch;
  std::uint64_t sequence = 0;
  std::uint64_t correlation = 0;
};

struct COHERENCE_API Frame {
  FrameHeader header;
  std::vector<std::byte> body;
};

COHERENCE_API std::vector<std::byte> encode_frame(const FrameHeader& header, ByteSpan body);
COHERENCE_API Status decode_frame_header(ByteSpan header_bytes, const FrameLimits& limits,
                                         FrameHeader& out);
COHERENCE_API Status read_frame(Socket& socket, const FrameLimits& limits, Frame& out);
COHERENCE_API Status write_frame(Socket& socket, const FrameHeader& header, ByteSpan body);

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
struct COHERENCE_API HelloRequest {
  std::uint32_t protocol_version = 0;
  std::uint32_t schema = 0;
  std::string build;
  std::string participant_name;
  ParticipantBootId boot;
  std::string node_label;
  std::uint64_t nonce = 0;
  /// When true the client is a read-only inspector and does not become a
  /// participant of the coherence domain.
  bool observer = false;
};

struct COHERENCE_API HelloResponse {
  SessionId session;
  CoordinatorEpoch epoch;
  std::uint32_t protocol_version = 0;
  std::uint32_t schema = 0;
  std::string build;
  std::uint64_t nonce = 0;
  ParticipantId participant;
  bool observer = false;
};

/// Generic response envelope. The status is always present so that a failure is
/// never mistaken for an empty success.
struct COHERENCE_API ResponseEnvelope {
  StatusCode code = StatusCode::Ok;
  std::string message;
  std::string detail;
};

COHERENCE_API void encode_hello_request(ByteWriter& w, const HelloRequest& value);
COHERENCE_API bool decode_hello_request(ByteReader& r, HelloRequest& out,
                                        const DecodeLimits& limits);
COHERENCE_API void encode_hello_response(ByteWriter& w, const HelloResponse& value);
COHERENCE_API bool decode_hello_response(ByteReader& r, HelloResponse& out,
                                         const DecodeLimits& limits);
COHERENCE_API void encode_response_envelope(ByteWriter& w, const ResponseEnvelope& value);
COHERENCE_API bool decode_response_envelope(ByteReader& r, ResponseEnvelope& out,
                                            const DecodeLimits& limits);

} // namespace coherence

#endif // COHERENCE_PROTOCOL_HPP
