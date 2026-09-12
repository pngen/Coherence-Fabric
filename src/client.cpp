// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/client.hpp"

#include <string>

#include "coherence/codec.hpp"
#include "coherence/version.hpp"

namespace coherence {

ControlClient::~ControlClient() { (void)close(); }

Status ControlClient::connect_and_handshake(const ClientOptions& options) {
  options_ = options;
  frames_ = options.frames;
  auto socket = Socket::connect(options.host, options.port);
  if (!socket.has_value()) return socket.status();
  socket_ = std::move(socket.value());

  HelloRequest hello;
  hello.protocol_version = protocol_version();
  hello.schema = persistence_schema();
  hello.build = build_identification();
  hello.participant_name = options.participant_name;
  hello.boot = options.boot;
  hello.node_label = options.node_label;
  hello.nonce = next_correlation_++;
  hello.observer = options.observer;

  ByteWriter writer;
  encode_hello_request(writer, hello);
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(protocol_version());
  header.type = MessageType::Hello;
  header.session = SessionId::nil();
  header.epoch = CoordinatorEpoch::nil();
  header.sequence = next_sequence_++;
  header.correlation = hello.nonce;
  Status sent = write_frame(socket_, header, writer.span());
  if (!sent.ok()) return sent;

  Frame frame;
  Status received = read_frame(socket_, frames_, frame);
  if (!received.ok()) return received;
  if (frame.header.type != MessageType::HelloAck) {
    if (frame.header.type == MessageType::Response) {
      ByteReader reader(frame.body);
      ResponseEnvelope envelope;
      if (decode_response_envelope(reader, envelope, default_decode_limits())) {
        return Status(envelope.code, "coordinator refused the handshake",
                      envelope.message + " " + envelope.detail);
      }
    }
    return Status(StatusCode::ProtocolViolation, "unexpected message during the handshake",
                  std::string(to_token(frame.header.type)));
  }
  ByteReader reader(frame.body);
  if (!decode_hello_response(reader, session_, default_decode_limits())) {
    return Status(StatusCode::ProtocolViolation, "handshake response could not be decoded");
  }
  if (session_.protocol_version != protocol_version()) {
    return Status(StatusCode::UnsupportedSchema, "coordinator protocol version mismatch",
                  "coordinator=" + std::to_string(session_.protocol_version) +
                      " local=" + std::to_string(protocol_version()));
  }
  return Status::success();
}

Result<Exchange> ControlClient::request(MessageType type, ByteSpan body) {
  if (!socket_.valid()) {
    return Result<Exchange>::failure(
        Status(StatusCode::TransportFailure, "the control client is not connected"));
  }
  FrameHeader header;
  header.version = static_cast<std::uint16_t>(protocol_version());
  header.type = type;
  header.session = session_.session;
  header.epoch = session_.epoch;
  header.sequence = next_sequence_++;
  header.correlation = next_correlation_++;
  Status sent = write_frame(socket_, header, body);
  if (!sent.ok()) return Result<Exchange>::failure(sent);

  Frame frame;
  Status received = read_frame(socket_, frames_, frame);
  if (!received.ok()) return Result<Exchange>::failure(received);
  if (frame.header.type != MessageType::Response) {
    return Result<Exchange>::failure(
        Status(StatusCode::ProtocolViolation, "coordinator replied with an unexpected message",
               std::string(to_token(frame.header.type))));
  }
  if (frame.header.correlation != header.correlation) {
    return Result<Exchange>::failure(Status(
        StatusCode::ProtocolViolation, "coordinator replied to a different request",
        "expected=" + std::to_string(header.correlation) +
            " received=" + std::to_string(frame.header.correlation)));
  }
  Exchange exchange;
  exchange.payload = std::move(frame.body);
  ByteReader reader(exchange.payload);
  if (!decode_response_envelope(reader, exchange.envelope, default_decode_limits())) {
    return Result<Exchange>::failure(
        Status(StatusCode::ProtocolViolation, "response envelope could not be decoded"));
  }
  const std::size_t consumed = reader.offset();
  exchange.payload.erase(exchange.payload.begin(),
                         exchange.payload.begin() + static_cast<std::ptrdiff_t>(consumed));
  return Result<Exchange>::success(std::move(exchange));
}

Result<Exchange> ControlClient::ping(std::uint64_t nonce) {
  ByteWriter writer;
  writer.u64(nonce);
  return request(MessageType::Ping, writer.span());
}

Status ControlClient::close() {
  if (socket_.valid()) {
    FrameHeader header;
    header.version = static_cast<std::uint16_t>(protocol_version());
    header.type = MessageType::Goodbye;
    header.session = session_.session;
    header.epoch = session_.epoch;
    header.sequence = next_sequence_++;
    header.correlation = next_correlation_++;
    (void)write_frame(socket_, header, {});
  }
  Status shut = socket_.shutdown();
  Status closed = socket_.close();
  return closed.ok() ? shut : closed;
}

bool decode_exchange_envelope(const Exchange& exchange, ResponseEnvelope& out) {
  // The control client decodes and validates the envelope as part of receiving
  // the response and strips it from the payload, so this is a plain accessor
  // rather than a second decode of the same bytes.
  out = exchange.envelope;
  return status_code_name(out.code) != std::string_view{"unknown_status"};
}

} // namespace coherence
