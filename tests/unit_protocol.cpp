// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Framing and protocol validation coverage, including malformed input.
#include <string>
#include <thread>
#include <vector>

#include "coherence/client.hpp"
#include "coherence/protocol.hpp"
#include "coherence/transport.hpp"
#include "coherence/version.hpp"
#include "test_framework.hpp"

namespace {

coherence::FrameHeader make_header(coherence::MessageType type) {
  coherence::FrameHeader header;
  header.version = static_cast<std::uint16_t>(coherence::protocol_version());
  header.type = type;
  header.session = coherence::generate_session_id();
  header.epoch = coherence::CoordinatorEpoch::from_value(3);
  header.sequence = 1;
  header.correlation = 42;
  return header;
}

coherence::ByteSpan as_bytes(const std::string& text) {
  return coherence::ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

} // namespace

CF_TEST(frames_round_trip_through_the_wire_encoding) {
  context.phase("SETUP");
  const std::string body = "payload-bytes";
  const coherence::FrameHeader header = make_header(coherence::MessageType::Ping);
  const std::vector<std::byte> encoded = coherence::encode_frame(header, as_bytes(body));
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(encoded.size()),
                 static_cast<std::uint64_t>(coherence::kFrameHeaderSize + body.size()));
  coherence::FrameHeader decoded;
  const coherence::ByteSpan head(encoded.data(), coherence::kFrameHeaderSize);
  CF_REQUIRE(coherence::decode_frame_header(head, coherence::FrameLimits{}, decoded).ok());
  CF_EXPECT(decoded.type == coherence::MessageType::Ping);
  CF_EXPECT(decoded.session == header.session);
  CF_EXPECT(decoded.epoch == header.epoch);
  CF_EXPECT_EQ_U(decoded.sequence, header.sequence);
  CF_EXPECT_EQ_U(decoded.correlation, header.correlation);
}

CF_TEST(frame_headers_reject_damage) {
  context.phase("SETUP");
  const coherence::FrameHeader header = make_header(coherence::MessageType::Ping);
  const std::vector<std::byte> encoded = coherence::encode_frame(header, as_bytes("x"));
  const coherence::ByteSpan head(encoded.data(), coherence::kFrameHeaderSize);

  // Magic mismatch.
  std::vector<std::byte> wrong_magic(encoded);
  coherence::store_u32(coherence::MutableByteSpan(wrong_magic), 0, 0xDEADBEEFu);
  coherence::FrameHeader ignored;
  CF_EXPECT(!coherence::decode_frame_header(
                 coherence::ByteSpan(wrong_magic.data(), coherence::kFrameHeaderSize),
                 coherence::FrameLimits{}, ignored)
                 .ok());

  // Protocol version mismatch.
  std::vector<std::byte> wrong_version(encoded);
  coherence::store_u16(coherence::MutableByteSpan(wrong_version), 4, 99);
  CF_EXPECT(!coherence::decode_frame_header(
                 coherence::ByteSpan(wrong_version.data(), coherence::kFrameHeaderSize),
                 coherence::FrameLimits{}, ignored)
                 .ok());

  // Undeclared message type.
  std::vector<std::byte> wrong_type(encoded);
  coherence::store_u16(coherence::MutableByteSpan(wrong_type), 6, 4000);
  const auto type_status = coherence::decode_frame_header(
      coherence::ByteSpan(wrong_type.data(), coherence::kFrameHeaderSize),
      coherence::FrameLimits{}, ignored);
  CF_EXPECT(!type_status.ok());
  CF_EXPECT(type_status.code() == coherence::StatusCode::ProtocolViolation);

  // Oversized declared body is rejected before reading anything.
  std::vector<std::byte> oversized(encoded);
  coherence::store_u32(coherence::MutableByteSpan(oversized), 52, 0x7FFFFFFFu);
  coherence::FrameLimits limits;
  limits.max_body_bytes = 4096;
  const auto size_status = coherence::decode_frame_header(
      coherence::ByteSpan(oversized.data(), coherence::kFrameHeaderSize), limits, ignored);
  CF_EXPECT(!size_status.ok());
  CF_EXPECT(size_status.code() == coherence::StatusCode::OversizedInput);

  // A short header is a truncation, not a valid frame.
  CF_EXPECT(!coherence::decode_frame_header(coherence::ByteSpan(head.data(), 12),
                                            coherence::FrameLimits{}, ignored)
                 .ok());
}

CF_TEST(frame_body_integrity_is_verified) {
  context.phase("CONNECT");
  coherence::initialize_networking();
  auto listener = coherence::Listener::bind("127.0.0.1", 0);
  CF_REQUIRE(listener.has_value());
  const std::uint16_t port = listener.value().port();

  std::thread server([&] {
    auto accepted = listener.value().accept();
    if (!accepted.has_value()) return;
    std::vector<std::byte> header_bytes(coherence::kFrameHeaderSize);
    if (!accepted.value().recv_exact(header_bytes).ok()) return;
    coherence::FrameHeader header;
    if (!coherence::decode_frame_header(header_bytes, coherence::FrameLimits{}, header).ok()) return;
    std::vector<std::byte> body(coherence::load_u32(header_bytes, 52));
    if (!body.empty() && !accepted.value().recv_exact(body).ok()) return;
    // Reply with a deliberately corrupted body.
    coherence::FrameHeader reply = header;
    reply.type = coherence::MessageType::Response;
    std::vector<std::byte> bytes = coherence::encode_frame(reply, as_bytes("hello"));
    bytes.back() = static_cast<std::byte>(std::to_integer<std::uint8_t>(bytes.back()) ^ 0xFFu);
    (void)accepted.value().send_all(bytes);
    (void)accepted.value().shutdown();
    (void)accepted.value().close();
  });

  auto socket = coherence::Socket::connect("127.0.0.1", port);
  CF_REQUIRE(socket.has_value());
  (void)socket.value().send_all(
      coherence::encode_frame(make_header(coherence::MessageType::Query), as_bytes("q")));
  coherence::Frame received;
  const coherence::Status status =
      coherence::read_frame(socket.value(), coherence::FrameLimits{}, received);
  CF_EXPECT(!status.ok());
  CF_EXPECT(status.code() == coherence::StatusCode::IntegrityFailure);
  (void)socket.value().close();
  (void)listener.value().interrupt();
  server.join();
}

CF_TEST(handshake_payloads_round_trip) {
  context.phase("SETUP");
  coherence::HelloRequest request;
  request.protocol_version = coherence::protocol_version();
  request.schema = coherence::persistence_schema();
  request.build = coherence::build_identification();
  request.participant_name = "alpha";
  request.boot = coherence::generate_participant_boot_id();
  request.node_label = "node";
  request.nonce = 0x1122334455667788ull;
  request.observer = false;
  coherence::ByteWriter writer;
  coherence::encode_hello_request(writer, request);
  coherence::HelloRequest decoded;
  coherence::ByteReader reader(writer.span());
  CF_REQUIRE(coherence::decode_hello_request(reader, decoded, coherence::default_decode_limits()));
  CF_EXPECT(reader.expect_end("hello").ok());
  CF_EXPECT_EQ(decoded.participant_name, request.participant_name);
  CF_EXPECT(decoded.boot == request.boot);
  CF_EXPECT_EQ_U(decoded.nonce, request.nonce);

  coherence::HelloResponse response;
  response.session = coherence::generate_session_id();
  response.epoch = coherence::CoordinatorEpoch::from_value(9);
  response.protocol_version = coherence::protocol_version();
  response.schema = coherence::persistence_schema();
  response.build = coherence::build_identification();
  response.nonce = 5;
  response.participant = coherence::ParticipantId::from_value(2);
  response.observer = true;
  coherence::ByteWriter response_writer;
  coherence::encode_hello_response(response_writer, response);
  coherence::HelloResponse response_decoded;
  coherence::ByteReader response_reader(response_writer.span());
  CF_REQUIRE(coherence::decode_hello_response(response_reader, response_decoded,
                                              coherence::default_decode_limits()));
  CF_EXPECT(response_decoded.session == response.session);
  CF_EXPECT(response_decoded.epoch == response.epoch);
  CF_EXPECT(response_decoded.observer);
}

CF_TEST(response_envelopes_reject_unknown_status_codes) {
  context.phase("SETUP");
  coherence::ByteWriter writer;
  writer.u16(60000);
  writer.text("boom");
  writer.text("");
  coherence::ResponseEnvelope envelope;
  coherence::ByteReader reader(writer.span());
  CF_EXPECT(!coherence::decode_response_envelope(reader, envelope, coherence::default_decode_limits()));

  coherence::ByteWriter good;
  coherence::encode_response_envelope(
      good, coherence::ResponseEnvelope{coherence::StatusCode::StaleBoot, "m", "d"});
  coherence::ResponseEnvelope decoded;
  coherence::ByteReader good_reader(good.span());
  CF_REQUIRE(coherence::decode_response_envelope(good_reader, decoded,
                                                 coherence::default_decode_limits()));
  CF_EXPECT(decoded.code == coherence::StatusCode::StaleBoot);
  CF_EXPECT_EQ(decoded.message, std::string("m"));
}

CF_TEST(message_type_tokens_are_stable_and_validated) {
  context.phase("SETUP");
  CF_EXPECT_EQ(std::string(coherence::to_token(coherence::MessageType::Hello)),
               std::string("hello"));
  CF_EXPECT_EQ(std::string(coherence::to_token(coherence::MessageType::Publish)),
               std::string("publish"));
  CF_EXPECT_EQ(std::string(coherence::to_token(coherence::MessageType::AcknowledgeInvalidation)),
               std::string("acknowledge_invalidation"));
  CF_EXPECT(coherence::message_type_from_u16(1).has_value());
  CF_EXPECT(!coherence::message_type_from_u16(0).has_value());
  CF_EXPECT(!coherence::message_type_from_u16(500).has_value());
}

CF_TEST(a_short_connection_is_reported_as_closed_not_as_a_protocol_fault) {
  context.phase("CONNECT");
  coherence::initialize_networking();
  auto listener = coherence::Listener::bind("127.0.0.1", 0);
  CF_REQUIRE(listener.has_value());
  std::thread server([&] {
    auto accepted = listener.value().accept();
    if (accepted.has_value()) {
      (void)accepted.value().close();
    }
  });
  auto socket = coherence::Socket::connect("127.0.0.1", listener.value().port());
  CF_REQUIRE(socket.has_value());
  coherence::Frame frame;
  const coherence::Status status =
      coherence::read_frame(socket.value(), coherence::FrameLimits{}, frame);
  CF_EXPECT(!status.ok());
  CF_EXPECT(status.code() == coherence::StatusCode::ConnectionClosed ||
            status.code() == coherence::StatusCode::TransportFailure);
  (void)socket.value().close();
  (void)listener.value().interrupt();
  server.join();
}

CF_TEST_MAIN()

