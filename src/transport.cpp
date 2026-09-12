// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/transport.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using native_socket_t = SOCKET;
constexpr native_socket_t kInvalidSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using native_socket_t = int;
constexpr native_socket_t kInvalidSocket = -1;
#endif

namespace coherence {
namespace {

std::once_flag g_network_once;

void shutdown_network_stack() {
#if defined(_WIN32)
  ::WSACleanup();
#endif
}

Status last_socket_error(StatusCode code, const char* what) {
#if defined(_WIN32)
  const int error = ::WSAGetLastError();
  return Status(code, what, "winsock=" + std::to_string(error));
#else
  return Status(code, what, std::string(std::strerror(errno)));
#endif
}

bool socket_valid(native_socket_t handle) noexcept { return handle != kInvalidSocket; }

} // namespace

void initialize_networking() {
  std::call_once(g_network_once, [] {
#if defined(_WIN32)
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) == 0) {
      std::atexit(&shutdown_network_stack);
    }
#endif
  });
}

Socket::~Socket() { (void)close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_), peer_(std::move(other.peer_)) {
  other.handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_ = other.handle_;
    peer_ = std::move(other.peer_);
    other.handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
  }
  return *this;
}

void Socket::reset() noexcept {
  handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
  peer_.clear();
}

bool Socket::valid() const noexcept {
  return handle_ != static_cast<std::uintptr_t>(kInvalidSocket);
}

Result<Socket> Socket::connect(const std::string& host, std::uint16_t port) {
  initialize_networking();
  ::addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  ::addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
    return Result<Socket>::failure(
        Status(StatusCode::TransportFailure, "cannot resolve host", host + ":" + service));
  }
  Status last = Status(StatusCode::TransportFailure, "no address could be reached");
  Socket socket;
  for (::addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    const native_socket_t handle =
        ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (!socket_valid(handle)) continue;
    if (::connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
      socket.handle_ = static_cast<std::uintptr_t>(handle);
      break;
    }
    last = last_socket_error(StatusCode::TransportFailure, "connect failed");
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
  }
  ::freeaddrinfo(results);
  if (!socket.valid()) return Result<Socket>::failure(last);
  (void)socket.set_no_delay(true);
  socket.peer_ = host + ":" + std::to_string(port);
  return Result<Socket>::success(std::move(socket));
}

Result<Socket> Socket::adopt(std::uintptr_t native_handle) {
  Socket socket;
  socket.handle_ = native_handle;
  return Result<Socket>::success(std::move(socket));
}

Status Socket::send_all(ByteSpan data) {
  if (!valid()) {
    return Status(StatusCode::TransportFailure, "send on a closed socket");
  }
  const native_socket_t handle = static_cast<native_socket_t>(handle_);
  const char* cursor = reinterpret_cast<const char*>(data.data());
  std::size_t remaining = data.size();
  while (remaining > 0) {
    const int chunk =
        static_cast<int>(remaining > 0x40000000ull ? 0x40000000ull : remaining);
#if defined(_WIN32)
    const int sent = ::send(handle, cursor, chunk, 0);
#else
    const int sent = static_cast<int>(::send(handle, cursor, static_cast<std::size_t>(chunk), 0));
#endif
    if (sent <= 0) {
      return last_socket_error(StatusCode::TransportFailure, "send failed");
    }
    cursor += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
  return Status::success();
}

Status Socket::recv_exact(MutableByteSpan out) {
  if (!valid()) {
    return Status(StatusCode::TransportFailure, "receive on a closed socket");
  }
  const native_socket_t handle = static_cast<native_socket_t>(handle_);
  char* cursor = reinterpret_cast<char*>(out.data());
  std::size_t remaining = out.size();
  while (remaining > 0) {
    const int chunk = static_cast<int>(remaining > 0x40000000ull ? 0x40000000ull : remaining);
#if defined(_WIN32)
    const int received = ::recv(handle, cursor, chunk, 0);
#else
    const int received = static_cast<int>(::recv(handle, cursor, static_cast<std::size_t>(chunk), 0));
#endif
    if (received == 0) {
      return Status(StatusCode::ConnectionClosed, "the peer closed the connection");
    }
    if (received < 0) {
      return last_socket_error(StatusCode::TransportFailure, "receive failed");
    }
    cursor += received;
    remaining -= static_cast<std::size_t>(received);
  }
  return Status::success();
}

Status Socket::set_no_delay(bool enabled) {
  if (!valid()) return Status(StatusCode::TransportFailure, "socket is closed");
  const int value = enabled ? 1 : 0;
  const char* data = reinterpret_cast<const char*>(&value);
  if (::setsockopt(static_cast<native_socket_t>(handle_), IPPROTO_TCP, TCP_NODELAY, data,
                   sizeof(value)) != 0) {
    return last_socket_error(StatusCode::TransportFailure, "setsockopt(TCP_NODELAY) failed");
  }
  return Status::success();
}

Status Socket::shutdown() {
  if (!valid()) return Status::success();
#if defined(_WIN32)
  (void)::shutdown(static_cast<native_socket_t>(handle_), SD_BOTH);
#else
  (void)::shutdown(static_cast<native_socket_t>(handle_), SHUT_RDWR);
#endif
  return Status::success();
}

Status Socket::close() {
  if (!valid()) {
    reset();
    return Status::success();
  }
  const native_socket_t handle = static_cast<native_socket_t>(handle_);
  reset();
#if defined(_WIN32)
  if (::closesocket(handle) != 0) {
    return last_socket_error(StatusCode::TransportFailure, "closesocket failed");
  }
#else
  if (::close(handle) != 0) {
    return last_socket_error(StatusCode::TransportFailure, "close failed");
  }
#endif
  return Status::success();
}

std::string Socket::peer() const { return peer_; }

Listener::~Listener() { (void)close(); }

Listener::Listener(Listener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
  other.handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
  other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
    other.port_ = 0;
  }
  return *this;
}

void Listener::reset() noexcept {
  handle_ = static_cast<std::uintptr_t>(kInvalidSocket);
  port_ = 0;
}

bool Listener::valid() const noexcept {
  return handle_ != static_cast<std::uintptr_t>(kInvalidSocket);
}

Result<Listener> Listener::bind(const std::string& host, std::uint16_t port, int backlog) {
  initialize_networking();
  ::addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  ::addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const char* node = host.empty() || host == "0.0.0.0" ? nullptr : host.c_str();
  if (::getaddrinfo(node, service.c_str(), &hints, &results) != 0) {
    return Result<Listener>::failure(
        Status(StatusCode::TransportFailure, "cannot resolve bind address", host));
  }
  Listener listener;
  Status last = Status(StatusCode::TransportFailure, "no address could be bound");
  for (::addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    const native_socket_t handle =
        ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (!socket_valid(handle)) continue;
    const int reuse = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (::bind(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) != 0) {
      last = last_socket_error(StatusCode::TransportFailure, "bind failed");
#if defined(_WIN32)
      ::closesocket(handle);
#else
      ::close(handle);
#endif
      continue;
    }
    if (::listen(handle, backlog) != 0) {
      last = last_socket_error(StatusCode::TransportFailure, "listen failed");
#if defined(_WIN32)
      ::closesocket(handle);
#else
      ::close(handle);
#endif
      continue;
    }
    ::sockaddr_storage bound{};
    int bound_length = static_cast<int>(sizeof(bound));
    if (::getsockname(handle, reinterpret_cast<::sockaddr*>(&bound), &bound_length) == 0) {
      if (bound.ss_family == AF_INET) {
        listener.port_ = ::ntohs(reinterpret_cast<::sockaddr_in*>(&bound)->sin_port);
      }
    }
    listener.handle_ = static_cast<std::uintptr_t>(handle);
    break;
  }
  ::freeaddrinfo(results);
  if (!listener.valid()) return Result<Listener>::failure(last);
  return Result<Listener>::success(std::move(listener));
}

Result<Socket> Listener::accept() {
  if (!valid()) {
    return Result<Socket>::failure(Status(StatusCode::TransportFailure, "listener is closed"));
  }
  ::sockaddr_storage address{};
  int length = static_cast<int>(sizeof(address));
  const native_socket_t handle =
      ::accept(static_cast<native_socket_t>(handle_), reinterpret_cast<::sockaddr*>(&address),
               &length);
  if (!socket_valid(handle)) {
    return Result<Socket>::failure(
        last_socket_error(StatusCode::TransportFailure, "accept failed"));
  }
  auto socket = Socket::adopt(static_cast<std::uintptr_t>(handle));
  (void)socket.value().set_no_delay(true);
  return socket;
}

Status Listener::interrupt() {
  if (!valid()) return Status::success();
#if defined(_WIN32)
  (void)::shutdown(static_cast<native_socket_t>(handle_), SD_BOTH);
#else
  (void)::shutdown(static_cast<native_socket_t>(handle_), SHUT_RDWR);
#endif
  return Status::success();
}

Status Listener::close() {
  if (!valid()) {
    reset();
    return Status::success();
  }
  const native_socket_t handle = static_cast<native_socket_t>(handle_);
  reset();
#if defined(_WIN32)
  if (::closesocket(handle) != 0) {
    return last_socket_error(StatusCode::TransportFailure, "closesocket failed");
  }
#else
  if (::close(handle) != 0) {
    return last_socket_error(StatusCode::TransportFailure, "close failed");
  }
#endif
  return Status::success();
}

Result<std::pair<std::string, std::uint16_t>> parse_endpoint(std::string_view text,
                                                             std::uint16_t default_port) {
  if (text.empty()) {
    return Result<std::pair<std::string, std::uint16_t>>::failure(
        Status(StatusCode::InvalidArgument, "endpoint text must not be empty"));
  }
  std::string host;
  std::uint16_t port = default_port;
  if (text.front() == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos) {
      return Result<std::pair<std::string, std::uint16_t>>::failure(
          Status(StatusCode::InvalidArgument, "bracketed endpoint is missing its closing bracket"));
    }
    host.assign(text.substr(1, close - 1));
    if (close + 1 < text.size()) {
      if (text[close + 1] != ':') {
        return Result<std::pair<std::string, std::uint16_t>>::failure(
            Status(StatusCode::InvalidArgument, "unexpected characters after the bracketed host"));
      }
      const std::string port_text(text.substr(close + 2));
      const long parsed = std::strtol(port_text.c_str(), nullptr, 10);
      if (parsed <= 0 || parsed > 65535) {
        return Result<std::pair<std::string, std::uint16_t>>::failure(
            Status(StatusCode::InvalidArgument, "port is out of range", port_text));
      }
      port = static_cast<std::uint16_t>(parsed);
    }
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
      host.assign(text);
    } else {
      host.assign(text.substr(0, colon));
      const std::string port_text(text.substr(colon + 1));
      const long parsed = std::strtol(port_text.c_str(), nullptr, 10);
      if (parsed <= 0 || parsed > 65535) {
        return Result<std::pair<std::string, std::uint16_t>>::failure(
            Status(StatusCode::InvalidArgument, "port is out of range", port_text));
      }
      port = static_cast<std::uint16_t>(parsed);
    }
  }
  if (host.empty()) host = "127.0.0.1";
  return Result<std::pair<std::string, std::uint16_t>>::success({host, port});
}

} // namespace coherence
