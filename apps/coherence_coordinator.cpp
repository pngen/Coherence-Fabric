// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// coherence_coordinator: the process that owns distributed coherence authority.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "coherence/coordinator.hpp"
#include "coherence/platform_file.hpp"
#include "coherence/version.hpp"

namespace {

coherence::Coordinator* g_coordinator = nullptr;

void handle_signal(int) {
  if (g_coordinator != nullptr) (void)g_coordinator->stop();
}

void usage() {
  std::fputs(
      "coherence_coordinator -- Coherence Fabric coordinator\n"
      "\n"
      "usage: coherence_coordinator [options]\n"
      "\n"
      "  --bind HOST            bind address (default 127.0.0.1)\n"
      "  --port N               TCP port; 0 selects an ephemeral port (default 0)\n"
      "  --state DIR            durable state directory; omit for an ephemeral coordinator\n"
      "  --domain NAME          coherence domain name (default 'default')\n"
      "  --node LABEL           node label used in inspection output\n"
      "  --max-sessions N       bound on concurrent sessions (default 256)\n"
      "  --ready-file PATH      write 'port=N\n' to PATH once the listener is bound\n"
      "  --accept-once          serve exactly one connection and then exit\n"
      "  --max-object-length N  bound on a logical object extent\n"
      "  --max-frame N          bound on a control frame body in bytes\n"
      "  --version              print the version and exit\n"
      "  --help                 print this help and exit\n",
      stdout);
}

bool next_value(int argc, char** argv, int& index, std::string& out) {
  if (index + 1 >= argc) return false;
  out = argv[++index];
  return true;
}

} // namespace

int main(int argc, char** argv) {
  coherence::CoordinatorOptions options;
  options.bind_host = "127.0.0.1";
  options.port = 0;
  bool accept_once = false;
  std::string ready_file;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string value;
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--version") {
      std::printf("coherence-fabric %s (%s)\n", coherence::version_string().c_str(),
                  coherence::build_identification());
      return 0;
    }
    if (arg == "--bind" && next_value(argc, argv, i, value)) {
      options.bind_host = value;
      continue;
    }
    if (arg == "--port" && next_value(argc, argv, i, value)) {
      options.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
      continue;
    }
    if (arg == "--state" && next_value(argc, argv, i, value)) {
      options.state_directory = std::filesystem::path(value);
      continue;
    }
    if (arg == "--domain" && next_value(argc, argv, i, value)) {
      options.domain_name = value;
      continue;
    }
    if (arg == "--node" && next_value(argc, argv, i, value)) {
      options.node_label = value;
      continue;
    }
    if (arg == "--max-sessions" && next_value(argc, argv, i, value)) {
      options.max_sessions = std::strtoull(value.c_str(), nullptr, 10);
      continue;
    }
    if (arg == "--max-object-length" && next_value(argc, argv, i, value)) {
      options.engine.max_object_length = std::strtoull(value.c_str(), nullptr, 10);
      continue;
    }
    if (arg == "--max-frame" && next_value(argc, argv, i, value)) {
      options.frames.max_body_bytes =
          static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
      continue;
    }
    if (arg == "--ready-file" && next_value(argc, argv, i, value)) {
      ready_file = value;
      continue;
    }
    if (arg == "--accept-once") {
      accept_once = true;
      continue;
    }
    std::fprintf(stderr, "coherence_coordinator: unrecognised argument '%s'\n", arg.c_str());
    return 2;
  }

  coherence::Coordinator coordinator(std::move(options));
  coherence::Status started = coordinator.start();
  if (!started.ok()) {
    std::fprintf(stderr, "coherence_coordinator: %s\n", started.to_string().c_str());
    return 1;
  }

  g_coordinator = &coordinator;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::printf("port=%u\n", static_cast<unsigned>(coordinator.port()));
  std::printf("epoch=%s\n", coordinator.engine().epoch().to_string().c_str());
  std::printf("domain=%s\n", coordinator.domain().to_string().c_str());
  std::fflush(stdout);

  if (!ready_file.empty()) {
    const std::string text = "port=" + std::to_string(coordinator.port()) + "\n";
    (void)coherence::atomic_replace_file(
        ready_file, coherence::ByteSpan(reinterpret_cast<const std::byte*>(text.data()),
                                        text.size()));
  }

  coherence::Status served = accept_once ? coordinator.accept_and_serve_one() : coordinator.run();
  (void)coordinator.stop();
  if (!served.ok() && served.code() != coherence::StatusCode::TransportFailure) {
    std::fprintf(stderr, "coherence_coordinator: %s\n", served.to_string().c_str());
  }
  std::printf("stopped=1\n");
  std::fflush(stdout);
  return 0;
}
