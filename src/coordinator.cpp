// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/coordinator.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "coherence/codec.hpp"
#include "coherence/evidence.hpp"
#include "coherence/persistence.hpp"
#include "coherence/version.hpp"
#include "control_codec.hpp"

namespace coherence {

struct Coordinator::Session {
  SessionId id;
  ParticipantId participant;
  ParticipantBootId boot;
  bool observer = false;
  bool replaced = false;
  bool said_goodbye = false;
  std::uint64_t last_sequence = 0;
  std::deque<std::uint64_t> recent_correlations;
  std::set<std::uint64_t> correlation_set;
  std::string peer;
  std::shared_ptr<Socket> socket;
  std::string name;
};

struct Coordinator::Impl {
  mutable std::mutex mutex;
  std::map<SessionId, std::shared_ptr<Session>> sessions;
  std::map<ParticipantId, SessionId> participant_session;
  std::vector<std::thread> threads;
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> rejected{0};
};

namespace {

void append_status(ByteWriter& body, const ResponseEnvelope& envelope) {
  encode_response_envelope(body, envelope);
}

ResponseEnvelope make_envelope(StatusCode code, std::string message, std::string detail = {}) {
  ResponseEnvelope envelope;
  envelope.code = code;
  envelope.message = std::move(message);
  envelope.detail = std::move(detail);
  return envelope;
}

ResponseEnvelope envelope_from(const Status& status) {
  ResponseEnvelope envelope;
  envelope.code = status.code();
  envelope.message = status.message();
  envelope.detail = status.detail();
  return envelope;
}

std::string render_policy_list(const CoherenceEngine& engine, ObjectId object) {
  auto policy = engine.get_policy(object);
  if (!policy.has_value()) return policy.status().to_string();
  return render_policy(policy.value());
}

} // namespace

Coordinator::Coordinator(CoordinatorOptions options)
    : options_(std::move(options)), impl_(std::make_unique<Impl>()) {
  if (options_.frames.max_body_bytes == 0) options_.frames.max_body_bytes = 16u * 1024u * 1024u;
  if (options_.state_directory.empty()) options_.engine.enable_durability = false;
}

Coordinator::~Coordinator() { (void)stop(); }

Status Coordinator::start() {
  if (started_) return Status::success();
  initialize_networking();

  if (!options_.state_directory.empty()) {
    options_.engine.enable_durability = true;
  }
  engine_ = std::make_unique<CoherenceEngine>(options_.engine);

  if (!options_.state_directory.empty()) {
    auto store = std::make_shared<FileDurableStore>(options_.state_directory);
    auto recovered = engine_->attach_store(store);
    if (!recovered.has_value()) return recovered.status();
    recovery_ = recovered.value();
  } else {
    recovery_.performed = false;
    recovery_.new_epoch = engine_->epoch();
    recovery_.notes.push_back(
        "no durable state directory was configured: publications are ephemeral and no authority "
        "survives a restart because none is recorded");
  }

  // The domain is established once and then recovered by identity.
  auto snapshot = engine_->snapshot(SnapshotOptions{});
  if (!snapshot.has_value()) return snapshot.status();
  domain_ = CoherenceDomainId::nil();
  for (const DomainRecord& record : snapshot.value().domains) {
    if (record.name == options_.domain_name) {
      domain_ = record.id;
      break;
    }
  }
  if (!domain_.defined()) {
    auto created = engine_->create_domain(options_.domain_name);
    if (!created.has_value()) return created.status();
    domain_ = created.value().id;
  }

  auto bound = Listener::bind(options_.bind_host, options_.port);
  if (!bound.has_value()) return bound.status();
  listener_ = std::move(bound.value());
  port_ = listener_.port();
  started_ = true;
  return Status::success();
}

std::uint64_t Coordinator::active_sessions() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->sessions.size();
}

Status Coordinator::run() {
  if (!started_) return Status(StatusCode::InvalidState, "the coordinator has not been started");
  while (!impl_->stopping.load(std::memory_order_acquire)) {
    auto accepted = listener_.accept();
    if (!accepted.has_value()) {
      if (impl_->stopping.load(std::memory_order_acquire)) break;
      continue;
    }
    Socket socket = std::move(accepted.value());
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->threads.emplace_back([this, sock = std::move(socket)]() mutable {
        (void)serve_socket(std::move(sock));
      });
    }
  }
  return Status::success();
}

Status Coordinator::accept_and_serve_one() {
  if (!started_) return Status(StatusCode::InvalidState, "the coordinator has not been started");
  auto accepted = listener_.accept();
  if (!accepted.has_value()) return accepted.status();
  Socket socket = std::move(accepted.value());
  return serve_socket(std::move(socket));
}

Status Coordinator::stop() {
  if (!started_ && !listener_.valid()) {
    // Still join anything that was started, so repeated stop() is safe.
  }
  impl_->stopping.store(true, std::memory_order_release);
  (void)listener_.interrupt();
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (auto& [id, session] : impl_->sessions) {
      (void)id;
      if (session->socket) (void)session->socket->shutdown();
    }
  }
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    threads.swap(impl_->threads);
  }
  for (std::thread& thread : threads) {
    if (thread.joinable()) thread.join();
  }
  (void)listener_.close();
  started_ = false;
  return Status::success();
}

Status Coordinator::shutdown_engine() {
  if (engine_ == nullptr) {
    return Status(StatusCode::InvalidState, "the coordinator has not been started");
  }
  Status begun = engine_->begin_shutdown(engine_->epoch());
  if (!begun.ok()) return begun;
  return engine_->complete_shutdown(engine_->epoch());
}

std::string Coordinator::render_status() const {
  std::string out;
  out.append("coordinator build=\"");
  out.append(build_identification());
  out.append("\"\n");
  out.append("  port=");
  out.append(std::to_string(port_));
  out.append(" bind=");
  out.append(options_.bind_host);
  out.append(" domain=");
  out.append(domain_.to_string());
  out.append(" name=");
  out.append(options_.domain_name);
  out.push_back('\n');
  out.append("  durable_state=");
  out.append(options_.state_directory.empty() ? "ephemeral" : options_.state_directory.string());
  out.push_back('\n');
  out.append("  epoch=");
  out.append(engine_ != nullptr ? engine_->epoch().to_string() : std::string("none"));
  out.append(" sequence=");
  out.append(engine_ != nullptr ? engine_->sequence().to_string() : std::string("none"));
  out.append(" sessions=");
  out.append(std::to_string(active_sessions()));
  out.append(" accepted=");
  out.append(std::to_string(impl_->accepted.load()));
  out.append(" rejected=");
  out.append(std::to_string(impl_->rejected.load()));
  out.push_back('\n');
  if (recovery_.performed) {
    out.append(recovery_.render());
  }
  return out;
}

// ---------------------------------------------------------------------------
// Connection handling
// ---------------------------------------------------------------------------
Status Coordinator::serve_socket(Socket socket) {
  auto session = std::make_shared<Session>();
  session->socket = std::make_shared<Socket>(std::move(socket));
  session->peer = session->socket->peer();
  impl_->accepted.fetch_add(1, std::memory_order_relaxed);

  auto refuse = [&](StatusCode code, const std::string& message,
                    const std::string& detail = std::string()) -> Status {
    ByteWriter body;
    append_status(body, make_envelope(code, message, detail));
    FrameHeader header;
    header.version = static_cast<std::uint16_t>(protocol_version());
    header.type = MessageType::Response;
    (void)write_frame(*session->socket, header, body.span());
    (void)session->socket->shutdown();
    (void)session->socket->close();
    impl_->rejected.fetch_add(1, std::memory_order_relaxed);
    return Status(code, message, detail);
  };

  // --- handshake -----------------------------------------------------------
  Frame hello_frame;
  Status read = read_frame(*session->socket, options_.frames, hello_frame);
  if (!read.ok()) {
    (void)session->socket->close();
    return read;
  }
  if (hello_frame.header.type != MessageType::Hello) {
    return refuse(StatusCode::ProtocolViolation, "the first frame must be a hello",
                  std::string(to_token(hello_frame.header.type)));
  }
  HelloRequest hello;
  {
    ByteReader reader(hello_frame.body);
    if (!decode_hello_request(reader, hello, default_decode_limits()) || !reader.at_end()) {
      return refuse(StatusCode::ProtocolViolation, "the hello frame could not be decoded");
    }
  }
  if (hello.protocol_version != protocol_version()) {
    return refuse(StatusCode::UnsupportedSchema, "protocol version mismatch",
                  "client=" + std::to_string(hello.protocol_version) +
                      " coordinator=" + std::to_string(protocol_version()));
  }
  if (hello.schema != persistence_schema()) {
    return refuse(StatusCode::UnsupportedSchema, "persistence schema mismatch",
                  "client=" + std::to_string(hello.schema) +
                      " coordinator=" + std::to_string(persistence_schema()));
  }
  if (engine_->shutting_down()) {
    return refuse(StatusCode::ShuttingDown, "the coordinator is shutting down");
  }

  const std::uint64_t max_sessions = options_.max_sessions == 0 ? 1 : options_.max_sessions;
  ParticipantRecord participant;
  bool observer = hello.observer;
  if (!observer) {
    if (hello.participant_name.empty()) {
      return refuse(StatusCode::InvalidArgument, "a participant name is required");
    }
    if (hello.boot.is_nil()) {
      return refuse(StatusCode::InvalidArgument,
                    "a participant boot identity is required");
    }
    // Connection replacement: an existing live session for this participant is
    // closed first so that two incarnations never hold authority at once.
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      if (impl_->sessions.size() >= max_sessions) {
        return refuse(StatusCode::CapacityExceeded, "session capacity reached");
      }
      auto existing = engine_->find_participant(hello.participant_name);
      if (existing.has_value()) {
        const auto it = impl_->participant_session.find(existing.value().id);
        if (it != impl_->participant_session.end()) {
          const auto old = impl_->sessions.find(it->second);
          if (old != impl_->sessions.end()) {
            old->second->replaced = true;
            if (old->second->socket) (void)old->second->socket->shutdown();
            impl_->sessions.erase(old);
            impl_->participant_session.erase(it);
          }
        }
        if (existing.value().boot != hello.boot) {
          // A new boot identity supersedes the previous incarnation. Anything
          // the previous incarnation still held is revoked before admission.
          (void)engine_->fence_participant(
              existing.value().id, engine_->epoch(),
              "superseded by a new participant boot identity");
        }
      }
    }
    auto registered = engine_->register_participant(hello.participant_name, hello.boot,
                                                    engine_->epoch(), hello.node_label);
    if (!registered.has_value()) {
      return refuse(registered.status().code(), registered.status().message(),
                    registered.status().detail());
    }
    participant = registered.value();
    session->participant = participant.id;
  }
  session->boot = hello.boot;
  session->observer = observer;
  session->name = hello.participant_name;
  session->id = generate_session_id();
  if (session->id.is_nil()) {
    return refuse(StatusCode::InternalError, "a session identity could not be generated");
  }
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->sessions.emplace(session->id, session);
    if (!observer) impl_->participant_session[participant.id] = session->id;
  }

  HelloResponse hello_response;
  hello_response.session = session->id;
  hello_response.epoch = engine_->epoch();
  hello_response.protocol_version = protocol_version();
  hello_response.schema = persistence_schema();
  hello_response.build = build_identification();
  hello_response.nonce = hello.nonce;
  hello_response.participant = participant.id;
  hello_response.observer = observer;
  {
    ByteWriter body;
    encode_hello_response(body, hello_response);
    FrameHeader header;
    header.version = static_cast<std::uint16_t>(protocol_version());
    header.type = MessageType::HelloAck;
    header.correlation = hello_frame.header.correlation;
    Status sent = write_frame(*session->socket, header, body.span());
    if (!sent.ok()) {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->sessions.erase(session->id);
      impl_->participant_session.erase(participant.id);
      (void)session->socket->close();
      return sent;
    }
  }

  // --- request loop --------------------------------------------------------
  Status outcome = Status::success();
  for (;;) {
    Frame frame;
    Status received = read_frame(*session->socket, options_.frames, frame);
    if (!received.ok()) {
      outcome = received;
      break;
    }
    if (frame.header.type == MessageType::Goodbye) {
      session->said_goodbye = true;
      break;
    }
    ResponseEnvelope envelope;
    ByteWriter body;
    bool closed = false;
    if (frame.header.session != session->id) {
      envelope = make_envelope(StatusCode::StaleSession,
                               "frame carries a session identity that is not the live session");
      closed = true;
    } else if (frame.header.sequence <= session->last_sequence) {
      // A replayed or reordered frame is a protocol violation: the stream is
      // strictly ordered and a repeated sequence means it was tampered with.
      envelope = make_envelope(StatusCode::ReplayedRequest,
                               "frame sequence did not advance; the request is a replay",
                               "received=" + std::to_string(frame.header.sequence) +
                                   " last=" + std::to_string(session->last_sequence));
      closed = true;
    } else if (session->correlation_set.count(frame.header.correlation) != 0) {
      envelope = make_envelope(StatusCode::ReplayedRequest,
                               "correlation identity was already used in this session");
      closed = true;
    } else if (frame.header.epoch != engine_->epoch()) {
      envelope = make_envelope(
          StatusCode::StaleEpoch,
          "request was issued under a coordinator epoch that is no longer current",
          "request=" + frame.header.epoch.to_string() +
              " current=" + engine_->epoch().to_string());
      session->last_sequence = frame.header.sequence;
    } else {
      session->last_sequence = frame.header.sequence;
      session->correlation_set.insert(frame.header.correlation);
      session->recent_correlations.push_back(frame.header.correlation);
      while (session->recent_correlations.size() > options_.correlation_window) {
        session->correlation_set.erase(session->recent_correlations.front());
        session->recent_correlations.pop_front();
      }
      Status dispatched = dispatch(session, frame, body, envelope);
      if (!dispatched.ok() && envelope.code == StatusCode::Ok) {
        envelope = envelope_from(dispatched);
      }
    }

    ByteWriter out;
    append_status(out, envelope);
    out.raw(body.span());
    FrameHeader header;
    header.version = static_cast<std::uint16_t>(protocol_version());
    header.type = MessageType::Response;
    header.session = session->id;
    header.epoch = engine_->epoch();
    header.correlation = frame.header.correlation;
    Status sent = write_frame(*session->socket, header, out.span());
    if (!sent.ok()) {
      outcome = sent;
      break;
    }
    if (closed) break;
  }

  // --- teardown ------------------------------------------------------------
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->sessions.erase(session->id);
    const auto it = impl_->participant_session.find(session->participant);
    if (it != impl_->participant_session.end() && it->second == session->id) {
      impl_->participant_session.erase(it);
    }
  }
  (void)session->socket->shutdown();
  (void)session->socket->close();

  if (observer || session->replaced) return Status::success();
  if (!session->participant.defined()) return Status::success();

  const auto still_live = engine_->get_participant(session->participant);
  if (!still_live.has_value()) return Status::success();
  if (still_live.value().boot != session->boot) return Status::success();

  if (session->said_goodbye) {
    (void)engine_->mark_session_closed(session->participant, session->boot, engine_->epoch());
  } else {
    // The connection disappeared without a goodbye: treat it as a participant
    // death. Authority held by that incarnation is fenced and never restored.
    (void)engine_->fence_participant(session->participant, engine_->epoch(),
                                     "the control connection was lost without a clean shutdown");
  }
  return outcome;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
Status Coordinator::dispatch(const std::shared_ptr<Session>& session, const Frame& frame,
                             ByteWriter& body, ResponseEnvelope& envelope) {
  using control::decode_create_domain;
  using control::decode_fence_participant;
  using control::decode_object_ref;
  using control::decode_register_object;
  using control::decode_region_ref;
  using control::decode_retire;
  using control::decode_set_policy;
  using control::decode_snapshot_options;
  using control::decode_text_block;
  using control::encode_create_domain;
  using control::encode_fence_participant;
  using control::encode_object_ref;
  using control::encode_register_object;
  using control::encode_region_ref;
  using control::encode_retire;
  using control::encode_set_policy;
  using control::encode_snapshot_options;
  using control::encode_text_block;

  const DecodeLimits& limits = default_decode_limits();
  CoherenceEngine& engine = *engine_;
  const CoordinatorEpoch epoch = engine.epoch();
  ByteReader reader(frame.body);

  auto fail = [&](const Status& status) {
    envelope = envelope_from(status);
    return status;
  };
  auto succeed = [&](std::string text) {
    envelope = make_envelope(StatusCode::Ok, "ok");
    encode_text_block(body, text);
    return Status::success();
  };

  // Observer sessions may inspect but never participate.
  const bool mutating = frame.header.type != MessageType::Query &&
                        frame.header.type != MessageType::Snapshot &&
                        frame.header.type != MessageType::Audit &&
                        frame.header.type != MessageType::Ping &&
                        frame.header.type != MessageType::ListParticipants &&
                        frame.header.type != MessageType::ListObjects &&
                        frame.header.type != MessageType::ListRegions &&
                        frame.header.type != MessageType::DecisionLog &&
                        frame.header.type != MessageType::RecoveryInfo;
  if (session->observer && mutating) {
    return fail(Status(StatusCode::NotWriteAuthorized,
                       "an observer session may inspect state but may not change it"));
  }

  switch (frame.header.type) {
    case MessageType::Ping: {
      std::uint64_t nonce = 0;
      if (!reader.u64(nonce)) return fail(reader.status());
      envelope = make_envelope(StatusCode::Ok, "pong");
      body.u64(nonce);
      body.strong_id(epoch);
      body.strong_id(engine.sequence());
      return Status::success();
    }
    case MessageType::CreateDomain: {
      std::string name;
      if (!decode_create_domain(reader, name, limits)) return fail(reader.status());
      auto created = engine.create_domain(name);
      if (!created.has_value()) return fail(created.status());
      encode_domain(body, created.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::RegisterObject: {
      CoherenceDomainId domain;
      std::string name;
      std::uint64_t length = 0;
      CoherencePolicy policy;
      if (!decode_register_object(reader, domain, name, length, policy, limits)) {
        return fail(reader.status());
      }
      auto object = engine.register_object(domain, name, length, policy);
      if (!object.has_value()) return fail(object.status());
      encode_object(body, object.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::RegisterRegion: {
      RegionRegistration registration;
      if (!decode_region_registration(reader, registration, limits)) {
        return fail(reader.status());
      }
      registration.context.participant = session->participant;
      registration.context.boot = session->boot;
      registration.context.epoch = epoch;
      auto region = engine.register_region(registration);
      if (!region.has_value()) return fail(region.status());
      encode_region(body, region.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::AcquireRead: {
      ReadRequest request;
      if (!decode_read_request(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto decision = engine.acquire_read(request);
      if (!decision.has_value()) return fail(decision.status());
      encode_read_decision(body, decision.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::AcquireWrite: {
      WriteRequest request;
      if (!decode_write_request(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto grant = engine.acquire_write(request);
      if (!grant.has_value()) return fail(grant.status());
      encode_write_grant(body, grant.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::MarkDirty: {
      DirtyRequest request;
      if (!decode_dirty_request(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto result = engine.mark_dirty(request);
      if (!result.has_value()) return fail(result.status());
      return succeed(std::string("dirty_condition=") + std::string(to_token(result.value())));
    }
    case MessageType::Publish: {
      PublishRequest request;
      if (!decode_publish_request(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto receipt = engine.publish(request);
      if (!receipt.has_value()) return fail(receipt.status());
      encode_publication(body, receipt.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::Invalidate: {
      InvalidationRequest request;
      if (!decode_invalidation_request(reader, request, limits)) return fail(reader.status());
      request.context.epoch = epoch;
      auto outcome = engine.invalidate(request);
      if (!outcome.has_value()) return fail(outcome.status());
      encode_invalidation_outcome(body, outcome.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::AcknowledgeInvalidation: {
      InvalidationAck acknowledgement;
      if (!decode_invalidation_ack(reader, acknowledgement, limits)) {
        return fail(reader.status());
      }
      acknowledgement.context.participant = session->participant;
      acknowledgement.context.boot = session->boot;
      acknowledgement.context.epoch = epoch;
      auto outcome = engine.acknowledge_invalidation(acknowledgement);
      if (!outcome.has_value()) return fail(outcome.status());
      encode_invalidation_outcome(body, outcome.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::SyncBegin: {
      SyncRequest request;
      if (!decode_sync_request(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto outcome = engine.begin_sync(request);
      if (!outcome.has_value()) return fail(outcome.status());
      encode_sync_outcome(body, outcome.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::SyncComplete: {
      SyncCompleteRequest request;
      if (!decode_sync_complete(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto outcome = engine.complete_sync(request);
      if (!outcome.has_value()) return fail(outcome.status());
      encode_sync_outcome(body, outcome.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::SyncFail: {
      SyncFailRequest request;
      if (!decode_sync_fail(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto outcome = engine.fail_sync(request);
      if (!outcome.has_value()) return fail(outcome.status());
      encode_sync_outcome(body, outcome.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::Release: {
      ReleaseRequest request;
      if (!decode_release_request(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto outcome = engine.release(request);
      if (!outcome.has_value()) return fail(outcome.status());
      encode_release_outcome(body, outcome.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::Revalidate: {
      RevalidateRequest request;
      if (!decode_revalidate_request(reader, request, limits)) return fail(reader.status());
      request.context.participant = session->participant;
      request.context.boot = session->boot;
      request.context.epoch = epoch;
      auto region = engine.revalidate_region(request);
      if (!region.has_value()) return fail(region.status());
      encode_region(body, region.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::ResolveRecovery: {
      ObjectId object;
      std::uint64_t generation = 0;
      std::string note;
      if (!decode_object_ref(reader, object)) return fail(reader.status());
      if (!reader.u64(generation)) return fail(reader.status());
      if (!decode_text_block(reader, note, limits)) return fail(reader.status());
      auto resolved = engine.resolve_recovery(
          object, coherence::ObjectGeneration::from_value(generation), epoch, note);
      if (!resolved.has_value()) return fail(resolved.status());
      encode_object(body, resolved.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::Snapshot: {
      SnapshotOptions options;
      if (!decode_snapshot_options(reader, options)) return fail(reader.status());
      auto snapshot = engine.snapshot(options);
      if (!snapshot.has_value()) return fail(snapshot.status());
      return succeed(snapshot.value().render());
    }
    case MessageType::Query: {
      ObjectId object;
      std::string selector;
      if (reader.remaining() >= 8) {
        if (!decode_object_ref(reader, object)) return fail(reader.status());
      }
      if (reader.remaining() > 0 && !decode_text_block(reader, selector, limits)) {
        return fail(reader.status());
      }
      if (!selector.empty() && selector == "policy") {
        return succeed(render_policy_list(engine, object));
      }
      if (!selector.empty() && selector == "audit") {
        return succeed(engine.audit().render());
      }
      if (!selector.empty() && selector == "decisions") {
        return succeed(engine.render_decision_log());
      }
      if (!selector.empty() && selector == "recovery") {
        return succeed(recovery_.render());
      }
      SnapshotOptions options;
      options.object_filter = object;
      auto snapshot = engine.snapshot(options);
      if (!snapshot.has_value()) return fail(snapshot.status());
      return succeed(snapshot.value().render());
    }
    case MessageType::Audit: {
      return succeed(engine.audit().render());
    }
    case MessageType::ListInvalidations: {
      // Only invalidations targeting this exact participant incarnation are
      // returned, so an acknowledgement can never be produced by the wrong
      // process or by a later incarnation of the same durable identity.
      auto snapshot = engine.snapshot(SnapshotOptions{});
      if (!snapshot.has_value()) return fail(snapshot.status());
      std::vector<const InvalidationRecord*> pending;
      for (const InvalidationRecord& record : snapshot.value().invalidations) {
        if (record.state != InvalidationState::Requested) continue;
        if (record.target_participant != session->participant) continue;
        if (record.target_boot != session->boot) continue;
        pending.push_back(&record);
      }
      body.u64(pending.size());
      for (const InvalidationRecord* record : pending) {
        encode_invalidation(body, *record);
      }
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::DecisionLog: {
      return succeed(engine.render_decision_log());
    }
    case MessageType::RecoveryInfo: {
      return succeed(recovery_.render());
    }
    case MessageType::ListParticipants:
    case MessageType::ListObjects:
    case MessageType::ListRegions: {
      auto snapshot = engine.snapshot(SnapshotOptions{});
      if (!snapshot.has_value()) return fail(snapshot.status());
      std::string out;
      if (frame.header.type == MessageType::ListParticipants) {
        for (const ParticipantRecord& record : snapshot.value().participants) {
          out.append(render_participant(record));
          out.push_back('\n');
        }
      } else if (frame.header.type == MessageType::ListObjects) {
        for (const ObjectRecord& record : snapshot.value().objects) {
          out.append(render_object(record));
          out.push_back('\n');
        }
      } else {
        for (const RegionRecord& record : snapshot.value().regions) {
          out.append(render_region(record));
          out.push_back('\n');
        }
      }
      return succeed(out);
    }
    case MessageType::SetPolicy: {
      ObjectId object;
      ObjectGeneration generation;
      PolicyGeneration expected;
      CoherencePolicy policy;
      if (!decode_set_policy(reader, object, generation, expected, policy, limits)) {
        return fail(reader.status());
      }
      auto updated = engine.set_policy(object, generation, expected, policy);
      if (!updated.has_value()) return fail(updated.status());
      encode_object(body, updated.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::FenceParticipant: {
      ParticipantId id;
      std::string reason;
      if (!decode_fence_participant(reader, id, reason, limits)) return fail(reader.status());
      auto fenced = engine.fence_participant(id, epoch, reason);
      if (!fenced.has_value()) return fail(fenced.status());
      // Any live session for that incarnation is closed.
      {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        const auto it = impl_->participant_session.find(id);
        if (it != impl_->participant_session.end()) {
          const auto live = impl_->sessions.find(it->second);
          if (live != impl_->sessions.end() && live->second->socket) {
            (void)live->second->socket->shutdown();
          }
        }
      }
      encode_participant(body, fenced.value());
      envelope = make_envelope(StatusCode::Ok, "ok");
      return Status::success();
    }
    case MessageType::RetireRegion: {
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!decode_retire(reader, id, generation)) return fail(reader.status());
      Status retired =
          engine.retire_region(RegionId::from_value(id), RegionGeneration::from_value(generation),
                               epoch);
      if (!retired.ok()) return fail(retired);
      return succeed("region " + std::to_string(id) + " retired");
    }
    case MessageType::RetireObject: {
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!decode_retire(reader, id, generation)) return fail(reader.status());
      Status retired = engine.retire_object(ObjectId::from_value(id),
                                            ObjectGeneration::from_value(generation), epoch);
      if (!retired.ok()) return fail(retired);
      return succeed("object " + std::to_string(id) + " retired");
    }
    case MessageType::Shutdown: {
      Status begun = engine.begin_shutdown(epoch);
      if (!begun.ok()) return fail(begun);
      envelope = make_envelope(StatusCode::Ok, "shutdown initiated");
      encode_text_block(body, "shutdown initiated: authority revoked and pending work settled");
      impl_->stopping.store(true, std::memory_order_release);
      (void)listener_.interrupt();
      return Status::success();
    }
    default:
      return fail(Status(StatusCode::Unsupported, "unsupported control message",
                         std::string(to_token(frame.header.type))));
  }
}

} // namespace coherence
