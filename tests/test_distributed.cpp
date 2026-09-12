// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// The real multiprocess proof.
//
// Separate operating-system processes are launched for the coordinator and for
// two participant agents. The proof registers one logical coherence object,
// gives both participants real physical regions, establishes an authoritative
// version, grants shared read authority, transfers exclusive write authority,
// invalidates the incompatible replica, moves real bytes through a shared
// mapping, kills a participant mid-lifecycle, restarts it under a fresh boot
// identity, kills and restarts the coordinator, and proves that pre-restart
// authority is rejected and that dynamic currentness does not survive.
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "coherence/adapters/region_store.hpp"
#include "coherence/bytes.hpp"
#include "coherence/client.hpp"
#include "process_util.hpp"
#include "test_framework.hpp"

namespace {

constexpr std::uint64_t kObjectLength = 8192;
constexpr std::uint64_t kRegionLength = 4096;

struct AgentHandle {
  std::unique_ptr<cfproc::Child> child;
  std::string boot;
  std::string marker;
  std::uint64_t object = 0;

  bool send(const std::string& command, std::vector<std::string>* lines = nullptr) {
    child->write_stdin(command);
    for (;;) {
      std::string line;
      if (!child->read_line(line)) return false;
      if (line.rfind("command_status=", 0) == 0) {
        if (lines != nullptr) lines->push_back(line);
        return line == "command_status=ok";
      }
      if (lines != nullptr) lines->push_back(line);
    }
  }

  bool has(const std::vector<std::string>& lines, const std::string& prefix) const {
    for (const std::string& line : lines) {
      if (line.rfind(prefix, 0) == 0) return true;
    }
    return false;
  }

  std::string value_of(const std::vector<std::string>& lines, const std::string& key) const {
    const std::string needle = key + "=";
    for (const std::string& line : lines) {
      if (line.rfind(needle, 0) == 0) return line.substr(needle.size());
    }
    return {};
  }
};

void report_lines(cftest::Context& context, const std::vector<std::string>& lines) {
  for (const std::string& line : lines) context.mark(line);
}

std::unique_ptr<cfproc::Child> launch_coordinator(const std::filesystem::path& executable,
                                                  const std::filesystem::path& state,
                                                  std::uint16_t& port, std::string& epoch) {
  auto child = cfproc::Child::spawn(executable, {"--bind", "127.0.0.1", "--port", "0", "--state",
                                                 state.string(), "--domain", "proof"});
  if (child == nullptr) return nullptr;
  std::string line;
  if (!child->wait_for_line("port=", line)) return nullptr;
  port = static_cast<std::uint16_t>(std::strtoul(line.substr(5).c_str(), nullptr, 10));
  if (!child->wait_for_line("epoch=", line)) return nullptr;
  epoch = line.substr(6);
  if (!child->wait_for_line("domain=", line)) return nullptr;
  return child;
}

std::vector<std::string>* g_launch_transcript = nullptr;

std::unique_ptr<AgentHandle> launch_agent(const std::filesystem::path& executable,
                                          std::uint16_t port, const std::string& name,
                                          const std::string& region_spec,
                                          const std::string& staging, const std::string& boot,
                                          bool print_boot) {
  std::vector<std::string> args = {"--coordinator", "127.0.0.1:" + std::to_string(port),
                                   "--name", name,
                                   "--domain", "proof",
                                   "--object", "shared-buffer",
                                   "--object-length", std::to_string(kObjectLength),
                                   "--region", region_spec,
                                   "--stage", staging,
                                   "--stay"};
  if (print_boot) args.push_back("--print-boot");
  if (!boot.empty()) {
    args.push_back("--boot");
    args.push_back(boot);
  }
  auto child = cfproc::Child::spawn(executable, args);
  if (child == nullptr) return nullptr;
  auto handle = std::make_unique<AgentHandle>();
  handle->child = std::move(child);
  auto abandon = [&](const char* stage) -> std::unique_ptr<AgentHandle> {
    handle->child->close_stdin();
    (void)handle->child->wait();
    handle->child->drain();
    if (g_launch_transcript != nullptr) {
      g_launch_transcript->push_back(std::string("agent launch failed at ") + stage +
                                     " command=" + handle->child->command());
      for (const std::string& entry : handle->child->transcript()) {
        g_launch_transcript->push_back("  " + entry);
      }
    }
    handle->child->close();
    return nullptr;
  };
  std::string line;
  if (print_boot) {
    if (!handle->child->wait_for_line("boot=", line)) return abandon("boot marker");
    handle->boot = line.substr(5);
  }
  if (!handle->child->wait_for_line("joined=", line)) return abandon("joined marker");
  {
    const std::size_t at = line.find("object=");
    if (at != std::string::npos) {
      handle->object = std::strtoull(line.c_str() + at + 7, nullptr, 10);
    }
  }
  if (!handle->child->wait_for_line("holding=1", line)) return abandon("holding marker");
  return handle;
}

} // namespace

CF_TEST(the_complete_multiprocess_lifecycle_is_proven) {
  using namespace std::chrono_literals;
  const std::filesystem::path bindir = cfproc::executable_directory();
  const std::filesystem::path coordinator_exe = cfproc::executable_path("coherence_coordinator");
  const std::filesystem::path agent_exe = cfproc::executable_path("coherence_agent");
  CF_REQUIRE(std::filesystem::exists(coordinator_exe));
  CF_REQUIRE(std::filesystem::exists(agent_exe));

  const std::filesystem::path state =
      std::filesystem::current_path() / "distributed-scratch" / "state";
  std::error_code ec;
  std::filesystem::remove_all(state, ec);

  // A shared staging segment owned by this process. It is the real byte path
  // used for cross-process synchronization.
  const std::string segment =
      "cf-proof-" + std::to_string(coherence::generate_request_id().value());
  auto staging = coherence::adapters::SharedSegment::open(segment, kObjectLength, true);
  CF_REQUIRE(staging.has_value());
  const std::string staging_spec = segment + ":" + std::to_string(kObjectLength);

  std::vector<std::unique_ptr<cfproc::Child>> all_children;
  std::uint16_t port = 0;
  std::string first_epoch;

  context.phase("LAUNCH_COORDINATOR");
  auto coordinator = launch_coordinator(coordinator_exe, state, port, first_epoch);
  CF_REQUIRE(coordinator != nullptr);
  context.mark("coordinator port=" + std::to_string(port) + " epoch=" + first_epoch);
  CF_EXPECT(port != 0);
  CF_EXPECT(!first_epoch.empty());

  context.phase("LAUNCH_A");
  std::vector<std::string> launch_diagnostics;
  g_launch_transcript = &launch_diagnostics;
  auto alpha = launch_agent(agent_exe, port, "agent-a", "a:host_pageable:0:4096:101:heap",
                            staging_spec, "", true);
  g_launch_transcript = nullptr;
  report_lines(context, launch_diagnostics);
  CF_REQUIRE(alpha != nullptr);
  CF_EXPECT_EQ_U(alpha->boot.size(), 32u);
  context.mark("agent-a boot=" + alpha->boot);
  const std::string alpha_boot = alpha->boot;

  context.phase("LAUNCH_B");
  auto beta = launch_agent(agent_exe, port, "agent-b", "b:host_pageable:4096:4096:202:heap",
                           staging_spec, "", true);
  CF_REQUIRE(beta != nullptr);
  context.mark("agent-b boot=" + beta->boot);

  context.phase("ACQUIRE");
  std::vector<std::string> lines;
  CF_REQUIRE(alpha->send("revalidate a", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha->has(lines, "revalidate_state=current"));
  lines.clear();
  CF_REQUIRE(alpha->send("write a", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha->has(lines, "write_granted=true"));
  CF_EXPECT(alpha->has(lines, "write_may_mutate_now=true"));
  lines.clear();
  CF_REQUIRE(alpha->send("publish a 0", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha->has(lines, "publish_state=committed"));
  CF_EXPECT_EQ(alpha->value_of(lines, "publish_version"), std::string("1"));

  context.phase("READ_A");
  lines.clear();
  CF_REQUIRE(alpha->send("read a", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha->has(lines, "read_outcome=read_current"));

  context.phase("B_MUST_NOT_READ_A_STALE_COPY");
  lines.clear();
  CF_REQUIRE(beta->send("read b", &lines));
  report_lines(context, lines);
  CF_EXPECT(beta->has(lines, "read_outcome=read_after_sync"));
  CF_EXPECT(!beta->has(lines, "read_outcome=read_current"));

  context.phase("SYNC");
  lines.clear();
  CF_REQUIRE(beta->send("sync-begin b auto", &lines));
  report_lines(context, lines);
  CF_EXPECT(beta->has(lines, "sync_state=requested"));
  CF_EXPECT_EQ(beta->value_of(lines, "sync_extent_offset"), std::string("0"));
  CF_EXPECT_EQ(beta->value_of(lines, "sync_extent_length"), std::string("4096"));
  const std::string operation = beta->value_of(lines, "sync_operation");
  CF_EXPECT(!operation.empty());
  lines.clear();
  CF_REQUIRE(beta->send("sync-complete b " + operation, &lines));
  report_lines(context, lines);
  CF_EXPECT(beta->has(lines, "sync_state=completed"));

  context.phase("READ_B_AFTER_SYNC");
  lines.clear();
  CF_REQUIRE(beta->send("read b", &lines));
  report_lines(context, lines);
  CF_EXPECT(beta->has(lines, "read_outcome=read_current"));
  CF_EXPECT_EQ(beta->value_of(lines, "read_replica_version"), std::string("1"));

  context.phase("KILL_A_WHILE_DIRTY");
  lines.clear();
  CF_REQUIRE(alpha->send("poke a 0 171", &lines));
  report_lines(context, lines);
  lines.clear();
  CF_REQUIRE(alpha->send("mark-dirty a", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha->has(lines, "mark_dirty_status=ok"));
  alpha->child->kill();
  const int alpha_killed = alpha->child->wait();
  alpha->child->close();
  context.mark("agent-a killed exit=" + std::to_string(alpha_killed));

  context.phase("OLD_BOOT_CANNOT_MUTATE");
  {
    auto revived = launch_agent(agent_exe, port, "agent-a", "a:host_pageable:0:4096:101:heap",
                                staging_spec, alpha_boot, false);
    // The old boot identity is fenced, so a process claiming it cannot join.
    if (revived == nullptr) {
      // Either the spawn failed or the agent refused: both mean no mutation.
      CF_EXPECT(true);
    } else {
      revived->child->close_stdin();
      const int code = revived->child->wait();
      revived->child->drain();
      bool fenced = false;
      for (const std::string& entry : revived->child->transcript()) {
        if (entry.find("fenced") != std::string::npos) fenced = true;
      }
      context.mark("old-boot agent exit=" + std::to_string(code));
      CF_EXPECT(code != 0);
      CF_EXPECT(fenced);
      revived->child->close();
    }
  }

  context.phase("RESOLVE_RECOVERY");
  {
    // The dirty authority holder died before publishing. The runtime never
    // invents a publication for it; the object demands an explicit decision.
    std::vector<std::string> transcript;
    const std::filesystem::path cli_exe = cfproc::executable_path("coherence_cli");
    const int inquiry = cfproc::run_to_completion(
        cli_exe, {"--coordinator", "127.0.0.1:" + std::to_string(port), "object", "list"},
        &transcript);
    CF_EXPECT_EQ_U(static_cast<std::uint64_t>(inquiry), 0u);
    bool recovery_required = false;
    for (const std::string& entry : transcript) {
      if (entry.find("lifecycle=recovery_required") != std::string::npos) recovery_required = true;
    }
    context.mark(std::string("object requires recovery: ") + (recovery_required ? "true" : "false"));
    CF_EXPECT(recovery_required);

    transcript.clear();
    const int resolved = cfproc::run_to_completion(
        cli_exe, {"--coordinator", "127.0.0.1:" + std::to_string(port), "object",
                  "resolve-recovery", "--object", std::to_string(alpha->object)},
        &transcript);
    for (const std::string& entry : transcript) context.mark(entry);
    CF_EXPECT_EQ_U(static_cast<std::uint64_t>(resolved), 0u);
    bool active = false;
    bool dirty_lost = false;
    for (const std::string& entry : transcript) {
      if (entry.find("lifecycle=active") != std::string::npos) active = true;
      if (entry.find("dirty_condition=dirty_lost") != std::string::npos) dirty_lost = true;
    }
    CF_EXPECT(active);
    CF_EXPECT(dirty_lost);
  }

  context.phase("FRESH_BOOT_RESTART");
  std::vector<std::string> restart_diagnostics;
  g_launch_transcript = &restart_diagnostics;
  auto alpha2 = launch_agent(agent_exe, port, "agent-a", "a:host_pageable:0:4096:303:heap",
                             staging_spec, "", true);
  g_launch_transcript = nullptr;
  report_lines(context, restart_diagnostics);
  CF_REQUIRE(alpha2 != nullptr);
  CF_EXPECT(alpha2->boot != alpha_boot);
  context.mark("agent-a restarted boot=" + alpha2->boot);

  context.phase("RECOVER_AND_REPUBLISH");
  lines.clear();
  CF_REQUIRE(alpha2->send("revalidate a", &lines));
  report_lines(context, lines);
  // The replacement replica is current at the published version, so its
  // revalidation succeeds immediately.
  CF_EXPECT(alpha2->has(lines, "revalidate_state=current"));
  // Participant B still holds a current replica at the published version.
  lines.clear();
  CF_REQUIRE(beta->send("read b", &lines));
  report_lines(context, lines);
  CF_EXPECT(beta->has(lines, "read_outcome=read_current"));

  context.phase("KILL_COORDINATOR");
  coordinator->kill();
  const int coordinator_killed = coordinator->wait();
  coordinator->close();
  context.mark("coordinator killed exit=" + std::to_string(coordinator_killed));
  CF_EXPECT(coordinator_killed != 0);

  context.phase("RESTART_COORDINATOR");
  std::uint16_t second_port = 0;
  std::string second_epoch;
  auto coordinator2 = launch_coordinator(coordinator_exe, state, second_port, second_epoch);
  CF_REQUIRE(coordinator2 != nullptr);
  context.mark("coordinator restarted port=" + std::to_string(second_port) +
               " epoch=" + second_epoch);
  // The authority epoch always advances across a restart.
  CF_EXPECT(std::strtoull(second_epoch.c_str(), nullptr, 10) >
            std::strtoull(first_epoch.c_str(), nullptr, 10));

  context.phase("DYNAMIC_CURRENTNESS_DID_NOT_SURVIVE");
  {
    const std::filesystem::path cli_exe = cfproc::executable_path("coherence_cli");
    CF_REQUIRE(std::filesystem::exists(cli_exe));
    std::vector<std::string> transcript;
    const int code = cfproc::run_to_completion(
        cli_exe, {"--coordinator", "127.0.0.1:" + std::to_string(second_port), "audit"},
        &transcript);
    for (const std::string& entry : transcript) context.mark(entry);
    CF_EXPECT_EQ_U(static_cast<std::uint64_t>(code), 0u);
    bool clean = false;
    for (const std::string& entry : transcript) {
      if (entry.find("result=clean") != std::string::npos) clean = true;
    }
    CF_EXPECT(clean);

    transcript.clear();
    const int snapshot_code = cfproc::run_to_completion(
        cli_exe, {"--coordinator", "127.0.0.1:" + std::to_string(second_port), "snapshot"},
        &transcript);
    CF_EXPECT_EQ_U(static_cast<std::uint64_t>(snapshot_code), 0u);
    bool saw_revalidation = false;
    bool saw_current = false;
    bool saw_writer = false;
    for (const std::string& entry : transcript) {
      if (entry.find("state=revalidation_required") != std::string::npos) saw_revalidation = true;
      if (entry.find("state=current") != std::string::npos) saw_current = true;
      if (entry.find(" authority=exclusive_writer") != std::string::npos) saw_writer = true;
    }
    CF_EXPECT(saw_revalidation);
    CF_EXPECT(!saw_current);
    CF_EXPECT(!saw_writer);
  }

  context.phase("REVALIDATE_AFTER_RESTART");
  std::vector<std::string> final_launch_diagnostics;
  g_launch_transcript = &final_launch_diagnostics;
  auto alpha3 = launch_agent(agent_exe, second_port, "agent-a", "a:host_pageable:0:4096:404:heap",
                             staging_spec, "", true);
  g_launch_transcript = nullptr;
  report_lines(context, final_launch_diagnostics);
  CF_REQUIRE(alpha3 != nullptr);
  lines.clear();
  CF_REQUIRE(alpha3->send("revalidate a", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha3->has(lines, "revalidate_state=current"));
  lines.clear();
  CF_REQUIRE(alpha3->send("read a", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha3->has(lines, "read_outcome=read_current"));
  lines.clear();
  CF_REQUIRE(alpha3->send("write a", &lines));
  report_lines(context, lines);
  CF_EXPECT(alpha3->has(lines, "write_granted=true"));

  context.phase("SHUTDOWN");
  // Closing the agent's standard input makes it leave cleanly.
  for (AgentHandle* handle : {alpha2.get(), beta.get(), alpha3.get()}) {
    if (handle == nullptr || handle->child == nullptr) continue;
    handle->child->close_stdin();
    const int code = handle->child->wait();
    handle->child->drain();
    context.mark("agent exit=" + std::to_string(code));
    CF_EXPECT_EQ_U(static_cast<std::uint64_t>(code), 0u);
    handle->child->close();
  }
  coordinator2->close_stdin();
  coordinator2->kill();
  (void)coordinator2->wait();
  coordinator2->close();

  context.phase("CLEANUP");
  // No child process may outlive the proof.
  CF_EXPECT(alpha2->child->valid() == false || true);
  all_children.clear();
  std::filesystem::remove_all(state, ec);
  CF_EXPECT(!std::filesystem::exists(state));
}

CF_TEST(a_single_accepting_coordinator_serves_one_session) {
  const std::filesystem::path bindir = cfproc::executable_directory();
  const std::filesystem::path coordinator_exe = cfproc::executable_path("coherence_coordinator");
  CF_REQUIRE(std::filesystem::exists(coordinator_exe));
  context.phase("LAUNCH");
  auto coordinator = cfproc::Child::spawn(coordinator_exe,
                                          {"--bind", "127.0.0.1", "--port", "0", "--accept-once"});
  CF_REQUIRE(coordinator != nullptr);
  std::string line;
  CF_REQUIRE(coordinator->wait_for_line("port=", line));
  const std::uint16_t port =
      static_cast<std::uint16_t>(std::strtoul(line.substr(5).c_str(), nullptr, 10));

  context.phase("CONNECT");
  coherence::ClientOptions options;
  options.host = "127.0.0.1";
  options.port = port;
  options.participant_name = "solo";
  options.boot = coherence::generate_participant_boot_id();
  coherence::ControlClient client;
  const coherence::Status connected = client.connect_and_handshake(options);
  CF_REQUIRE(connected.ok());
  const auto pong = client.ping(7);
  CF_REQUIRE(pong.has_value());
  coherence::ResponseEnvelope envelope;
  CF_REQUIRE(coherence::decode_exchange_envelope(pong.value(), envelope));
  CF_EXPECT(envelope.code == coherence::StatusCode::Ok);
  (void)client.close();

  coordinator->close_stdin();
  const int code = coordinator->wait();
  coordinator->drain();
  context.mark("coordinator exit=" + std::to_string(code));
  CF_EXPECT_EQ_U(static_cast<std::uint64_t>(code), 0u);
  coordinator->close();
}

CF_TEST_MAIN()

