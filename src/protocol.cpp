// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/protocol.hpp"

#include <cstring>
#include <optional>

#include "coherence/codec.hpp"
#include "coherence/version.hpp"

namespace coherence {

std::string_view to_token(MessageType value) noexcept {
  switch (value) {
    case MessageType::Invalid: return "invalid";
    case MessageType::Hello: return "hello";
    case MessageType::HelloAck: return "hello_ack";
    case MessageType::Response: return "response";
    case MessageType::Goodbye: return "goodbye";
    case MessageType::CreateDomain: return "create_domain";
    case MessageType::RegisterParticipant: return "register_participant";
    case MessageType::RegisterObject: return "register_object";
    case MessageType::RegisterRegion: return "register_region";
    case MessageType::AcquireRead: return "acquire_read";
    case MessageType::AcquireWrite: return "acquire_write";
    case MessageType::MarkDirty: return "mark_dirty";
    case MessageType::Publish: return "publish";
    case MessageType::Invalidate: return "invalidate";
    case MessageType::AcknowledgeInvalidation: return "acknowledge_invalidation";
    case MessageType::SyncBegin: return "sync_begin";
    case MessageType::SyncComplete: return "sync_complete";
    case MessageType::SyncFail: return "sync_fail";
    case MessageType::Release: return "release";
    case MessageType::Revalidate: return "revalidate";
    case MessageType::Query: return "query";
    case MessageType::Snapshot: return "snapshot";
    case MessageType::Audit: return "audit";
    case MessageType::FenceParticipant: return "fence_participant";
    case MessageType::RetireRegion: return "retire_region";
    case MessageType::RetireObject: return "retire_object";
    case MessageType::Shutdown: return "shutdown";
    case MessageType::Ping: return "ping";
    case MessageType::SetPolicy: return "set_policy";
    case MessageType::ListParticipants: return "list_participants";
    case MessageType::ListObjects: return "list_objects";
    case MessageType::ListRegions: return "list_regions";
    case MessageType::DecisionLog: return "decision_log";
    case MessageType::RecoveryInfo: return "recovery_info";
    case MessageType::DrainSessions: return "drain_sessions";
    case MessageType::ListInvalidations: return "list_invalidations";
    case MessageType::ResolveRecovery: return "resolve_recovery";
  }
  return "unknown";
}

std::optional<MessageType> message_type_from_u16(std::uint16_t raw) noexcept {
  switch (raw) {
    case 1: return MessageType::Hello;
    case 2: return MessageType::HelloAck;
    case 3: return MessageType::Response;
    case 4: return MessageType::Goodbye;
    case 10: return MessageType::CreateDomain;
    case 11: return MessageType::RegisterParticipant;
    case 12: return MessageType::RegisterObject;
    case 13: return MessageType::RegisterRegion;
    case 14: return MessageType::AcquireRead;
    case 15: return MessageType::AcquireWrite;
    case 16: return MessageType::MarkDirty;
    case 17: return MessageType::Publish;
    case 18: return MessageType::Invalidate;
    case 19: return MessageType::AcknowledgeInvalidation;
    case 20: return MessageType::SyncBegin;
    case 21: return MessageType::SyncComplete;
    case 22: return MessageType::SyncFail;
    case 23: return MessageType::Release;
    case 24: return MessageType::Revalidate;
    case 25: return MessageType::Query;
    case 26: return MessageType::Snapshot;
    case 27: return MessageType::Audit;
    case 28: return MessageType::FenceParticipant;
    case 29: return MessageType::RetireRegion;
    case 30: return MessageType::RetireObject;
    case 31: return MessageType::Shutdown;
    case 32: return MessageType::Ping;
    case 33: return MessageType::SetPolicy;
    case 34: return MessageType::ListParticipants;
    case 35: return MessageType::ListObjects;
    case 36: return MessageType::ListRegions;
    case 37: return MessageType::DecisionLog;
    case 38: return MessageType::RecoveryInfo;
    case 39: return MessageType::DrainSessions;
    case 40: return MessageType::ListInvalidations;
    case 41: return MessageType::ResolveRecovery;
    default: return std::nullopt;
  }
}

std::vector<std::byte> encode_frame(const FrameHeader& header, ByteSpan body) {
  std::vector<std::byte> out(kFrameHeaderSize + body.size());
  const MutableByteSpan head(out.data(), kFrameHeaderSize);
  store_u32(head, 0, kFrameMagic);
  store_u16(head, 4, header.version);
  store_u16(head, 6, static_cast<std::uint16_t>(header.type));
  store_u32(head, 8, header.flags);
  store_u64(head, 12, header.session.value().high);
  store_u64(head, 20, header.session.value().low);
  store_u64(head, 28, header.epoch.value());
  store_u64(head, 36, header.sequence);
  store_u64(head, 44, header.correlation);
  store_u32(head, 52, static_cast<std::uint32_t>(body.size()));
  store_u32(head, 56, crc32c(body));
  if (!body.empty()) {
    std::memcpy(out.data() + kFrameHeaderSize, body.data(), body.size());
  }
  return out;
}

Status decode_frame_header(ByteSpan header_bytes, const FrameLimits& limits, FrameHeader& out) {
  if (header_bytes.size() < kFrameHeaderSize) {
    return Status(StatusCode::TruncatedInput, "frame header is incomplete");
  }
  if (load_u32(header_bytes, 0) != kFrameMagic) {
    return Status(StatusCode::ProtocolViolation, "frame magic mismatch");
  }
  const std::uint16_t version = load_u16(header_bytes, 4);
  if (version != static_cast<std::uint16_t>(protocol_version())) {
    return Status(StatusCode::UnsupportedSchema, "control-plane protocol version mismatch",
                  "peer=" + std::to_string(version) +
                      " local=" + std::to_string(protocol_version()));
  }
  const auto type = message_type_from_u16(load_u16(header_bytes, 6));
  if (!type.has_value()) {
    return Status(StatusCode::ProtocolViolation, "frame carries an undeclared message type");
  }
  const std::uint32_t body_length = load_u32(header_bytes, 52);
  if (body_length > limits.max_body_bytes) {
    return Status(StatusCode::OversizedInput, "frame body exceeds the configured bound",
                  "declared=" + std::to_string(body_length) +
                      " bound=" + std::to_string(limits.max_body_bytes));
  }
  out.version = version;
  out.type = *type;
  out.flags = load_u32(header_bytes, 8);
  out.session = SessionId::from_value(
      UInt128{load_u64(header_bytes, 12), load_u64(header_bytes, 20)});
  out.epoch = CoordinatorEpoch::from_value(load_u64(header_bytes, 28));
  out.sequence = load_u64(header_bytes, 36);
  out.correlation = load_u64(header_bytes, 44);
  return Status::success();
}

Status read_frame(Socket& socket, const FrameLimits& limits, Frame& out) {
  std::vector<std::byte> head(kFrameHeaderSize);
  Status read = socket.recv_exact(head);
  if (!read.ok()) return read;
  Status decoded = decode_frame_header(head, limits, out.header);
  if (!decoded.ok()) return decoded;
  const std::uint32_t body_length = load_u32(head, 52);
  const std::uint32_t body_crc = load_u32(head, 56);
  out.body.assign(body_length, std::byte{0});
  if (body_length != 0) {
    Status read_body = socket.recv_exact(out.body);
    if (!read_body.ok()) return read_body;
  }
  if (crc32c(out.body) != body_crc) {
    return Status(StatusCode::IntegrityFailure, "frame body failed its CRC-32C check");
  }
  return Status::success();
}

Status write_frame(Socket& socket, const FrameHeader& header, ByteSpan body) {
  const std::vector<std::byte> bytes = encode_frame(header, body);
  return socket.send_all(bytes);
}

void encode_hello_request(ByteWriter& w, const HelloRequest& value) {
  w.u32(value.protocol_version);
  w.u32(value.schema);
  w.text(value.build);
  w.text(value.participant_name);
  w.strong_id(value.boot);
  w.text(value.node_label);
  w.u64(value.nonce);
  w.boolean(value.observer);
}

bool decode_hello_request(ByteReader& r, HelloRequest& out, const DecodeLimits& limits) {
  HelloRequest value;
  if (!r.u32(value.protocol_version)) return false;
  if (!r.u32(value.schema)) return false;
  if (!r.text(value.build, limits.max_text)) return false;
  if (!r.text(value.participant_name, limits.max_text)) return false;
  if (!r.strong_id(value.boot)) return false;
  if (!r.text(value.node_label, limits.max_text)) return false;
  if (!r.u64(value.nonce)) return false;
  if (!r.boolean(value.observer)) return false;
  out = std::move(value);
  return true;
}

void encode_hello_response(ByteWriter& w, const HelloResponse& value) {
  w.strong_id(value.session);
  w.strong_id(value.epoch);
  w.u32(value.protocol_version);
  w.u32(value.schema);
  w.text(value.build);
  w.u64(value.nonce);
  w.strong_id(value.participant);
  w.boolean(value.observer);
}

bool decode_hello_response(ByteReader& r, HelloResponse& out, const DecodeLimits& limits) {
  HelloResponse value;
  if (!r.strong_id(value.session)) return false;
  if (!r.strong_id(value.epoch)) return false;
  if (!r.u32(value.protocol_version)) return false;
  if (!r.u32(value.schema)) return false;
  if (!r.text(value.build, limits.max_text)) return false;
  if (!r.u64(value.nonce)) return false;
  if (!r.strong_id(value.participant)) return false;
  if (!r.boolean(value.observer)) return false;
  out = std::move(value);
  return true;
}

void encode_response_envelope(ByteWriter& w, const ResponseEnvelope& value) {
  w.u16(static_cast<std::uint16_t>(value.code));
  w.text(value.message);
  w.text(value.detail);
}

bool decode_response_envelope(ByteReader& r, ResponseEnvelope& out, const DecodeLimits& limits) {
  std::uint16_t raw = 0;
  if (!r.u16(raw)) return false;
  const StatusCode code = static_cast<StatusCode>(raw);
  if (status_code_name(code) == std::string_view{"unknown_status"}) return false;
  ResponseEnvelope value;
  value.code = code;
  if (!r.text(value.message, limits.max_text)) return false;
  if (!r.text(value.detail, limits.max_text)) return false;
  out = std::move(value);
  return true;
}

} // namespace coherence
