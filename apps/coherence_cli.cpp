// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// coherence_cli: deterministic inspection and proof driver.
//
// Two modes:
//   embedded (default)  -- the CLI owns a real coherence engine and a real
//                          region store, so its demonstrations manipulate real
//                          bytes in this process.
//   --coordinator H:P   -- the CLI attaches to a running coordinator. State
//                          changing commands are issued through a real control
//                          session, so the CLI is a genuine participant.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "coherence/adapters/region_store.hpp"
#include "coherence/client.hpp"
#include "coherence/codec.hpp"
#include "coherence/engine.hpp"
#include "coherence/persistence.hpp"
#include "coherence/transport.hpp"
#include "coherence/version.hpp"

namespace {

using coherence::Result;
using coherence::Status;
using coherence::StatusCode;

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> flags;
  std::vector<std::string> repeated_command;

  [[nodiscard]] bool has(const std::string& key) const { return flags.count(key) != 0; }
  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = {}) const {
    const auto it = flags.find(key);
    return it == flags.end() ? fallback : it->second;
  }
  [[nodiscard]] std::uint64_t number(const std::string& key, std::uint64_t fallback = 0) const {
    if (!has(key)) return fallback;
    return std::strtoull(flags.at(key).c_str(), nullptr, 10);
  }
};

void usage() {
  std::fputs(
      "coherence_cli -- Coherence Fabric inspection and proof driver\n"
      "\n"
      "usage: coherence_cli [--state DIR] [--coordinator HOST:PORT] COMMAND [options]\n"
      "\n"
      "commands:\n"
      "  status\n"
      "  domain create --name NAME\n"
      "  domain show\n"
      "  object create --domain ID --name NAME --length N [--consistency MODEL]\n"
      "                [--stale-read POLICY] [--stale-bound N] [--durability MODE]\n"
      "                [--ownership MODE] [--recovery MODE]\n"
      "  object show --object ID\n"
      "  object list\n"
      "  object retire --object ID --generation G\n"
      "  policy show --object ID\n"
      "  region register --object ID --name NAME --length N [--offset O]\n"
      "                  [--memory-domain DOMAIN] [--evidence CLASS] [--address N]\n"
      "  region list [--object ID]\n"
      "  region retire --region ID --generation G\n"
      "  acquire read --object ID [--region ID] [--snapshot-version V]\n"
      "  acquire write --object ID [--region ID]\n"
      "  mark-dirty --object ID --region ID --ownership G --base-version V\n"
      "  publish --object ID --region ID --ownership G --base-version V [--allow-initial]\n"
      "  invalidate --object ID --region ID\n"
      "  ack-invalidation --invalidation ID --region ID --region-generation G\n"
      "                   --replica-generation G\n"
      "  sync-begin --object ID --destination ID [--source ID]\n"
      "  sync-complete --operation ID --destination ID --version V\n"
      "  release --object ID (--lease L | --write --ownership G)\n"
      "  revalidate --object ID --region ID --version V\n"
      "  participant register --name NAME\n"
      "  participant list\n"
      "  participant show --id P\n"
      "  participant fence --id P [--reason TEXT]\n"
      "  snapshot [--object ID]\n"
      "  decisions\n"
      "  recovery\n"
      "  audit\n"
      "  verify\n"
      "  demo [--keep]\n"
      "  help\n",
      stdout);
}

// ---------------------------------------------------------------------------
// Embedded mode
// ---------------------------------------------------------------------------
struct Embedded {
  coherence::EngineConfig engine_config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::adapters::RegionStore store;
  coherence::ParticipantRecord participant;
  coherence::CoherenceDomainId domain;
  std::map<std::string, coherence::RegionId> region_names;
  std::shared_ptr<coherence::DurableStore> durable;
  int failures = 0;
};

void emit(const std::string& text) {
  std::printf("%s\n", text.c_str());
  std::fflush(stdout);
}

void emit_failure(const Status& status) {
  std::printf("error code=%s message=%s\n",
              std::string(coherence::status_code_name(status.code())).c_str(),
              status.message().c_str());
  std::fflush(stdout);
}

coherence::CoherencePolicy build_policy(const Args& args) {
  const std::string model = args.get("consistency", "strict");
  coherence::CoherencePolicy policy = [&] {
    if (model == "release_acquire") return coherence::release_acquire_policy("cli");
    if (model == "snapshot") return coherence::snapshot_policy("cli");
    if (model == "eventual") {
      return coherence::eventual_policy(args.number("stale-bound", 8), "cli");
    }
    return coherence::strict_policy("cli");
  }();
  if (args.has("stale-read")) {
    const auto parsed = coherence::parse_stale_read_policy(args.get("stale-read"));
    if (parsed.has_value()) policy.stale_read_policy = parsed.value();
  }
  if (args.has("durability")) {
    const auto parsed = coherence::parse_publication_durability(args.get("durability"));
    if (parsed.has_value()) policy.publication_durability = parsed.value();
  }
  if (args.has("ownership")) {
    const auto parsed = coherence::parse_write_ownership_mode(args.get("ownership"));
    if (parsed.has_value()) policy.write_ownership = parsed.value();
  }
  if (args.has("recovery")) {
    const auto parsed = coherence::parse_recovery_policy(args.get("recovery"));
    if (parsed.has_value()) policy.recovery_policy = parsed.value();
  }
  if (policy.stale_read_policy == coherence::StaleReadPolicy::Never &&
      policy.consistency == coherence::ConsistencyModel::Eventual) {
    policy.stale_read_policy = coherence::StaleReadPolicy::Bounded;
  }
  if (policy.consistency == coherence::ConsistencyModel::Strict &&
      policy.stale_read_policy != coherence::StaleReadPolicy::Never) {
    policy.consistency = coherence::ConsistencyModel::Eventual;
  }
  return policy;
}

bool initialise_embedded(Embedded& state, const Args& args) {
  state.engine = std::make_unique<coherence::CoherenceEngine>(state.engine_config);
  if (args.has("state")) {
    auto store = std::make_shared<coherence::FileDurableStore>(args.get("state"));
    auto recovered = state.engine->attach_store(store);
    if (!recovered.has_value()) {
      emit_failure(recovered.status());
      return false;
    }
    state.durable = store;
    for (const std::string& note : recovered.value().notes) emit("recovery note=" + note);
  }
  const coherence::ParticipantBootId boot = coherence::generate_participant_boot_id();
  auto participant = state.engine->register_participant("cli", boot, state.engine->epoch(), "cli");
  if (!participant.has_value()) {
    emit_failure(participant.status());
    return false;
  }
  state.participant = participant.value();
  auto snapshot = state.engine->snapshot(coherence::SnapshotOptions{});
  if (snapshot.has_value() && !snapshot.value().domains.empty()) {
    state.domain = snapshot.value().domains.front().id;
  }
  return true;
}

coherence::AuthorityContext authority_for(Embedded& state, coherence::ObjectId object) {
  coherence::AuthorityContext context;
  context.epoch = state.engine->epoch();
  context.participant = state.participant.id;
  context.boot = state.participant.boot;
  context.object = object;
  auto record = state.engine->get_object(object);
  if (record.has_value()) {
    context.object_generation = record.value().generation;
    context.policy_generation = record.value().policy_generation;
  }
  context.request = coherence::generate_request_id();
  return context;
}

int run_embedded(Embedded& state, const Args& args);
int run_demo(Embedded& state, const Args& args);

// ---------------------------------------------------------------------------
// Remote mode
// ---------------------------------------------------------------------------
struct Remote {
  coherence::ControlClient client;
  coherence::ParticipantBootId boot;
};

int run_remote(Remote& state, const Args& args);

bool initialise_remote(Remote& state, const Args& args) {
  const auto endpoint = coherence::parse_endpoint(args.get("coordinator"), 0);
  if (!endpoint.has_value()) {
    emit_failure(endpoint.status());
    return false;
  }
  coherence::ClientOptions options;
  options.host = endpoint.value().first;
  options.port = endpoint.value().second;
  // Read-only commands run as an observer session; commands that change state
  // run as a real participant.
  const std::string group = args.positional.empty() ? std::string() : args.positional[0];
  const bool mutating = group == "object" || group == "domain" || group == "region" ||
                        group == "acquire" || group == "mark-dirty" || group == "publish" ||
                        group == "invalidate" || group == "ack-invalidation" ||
                        group == "sync-begin" || group == "sync-complete" || group == "sync-fail" ||
                        group == "release" || group == "revalidate" || group == "participant";
  options.observer = !mutating;
  options.participant_name = args.get("name", "cli-inspector");
  options.boot = coherence::generate_participant_boot_id();
  options.node_label = "cli";
  const Status connected = state.client.connect_and_handshake(options);
  if (!connected.ok()) {
    emit_failure(connected);
    return false;
  }
  state.boot = options.boot;
  return true;
}

#include "cli_embedded.inc"
#include "cli_remote.inc"
#include "cli_demo.inc"

} // namespace

int main(int argc, char** argv) {
  coherence::initialize_networking();
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
      std::string key = arg.substr(2);
      std::string value;
      const std::size_t equals = key.find('=');
      if (equals != std::string::npos) {
        value = key.substr(equals + 1);
        key = key.substr(0, equals);
      } else if (i + 1 < argc && argv[i + 1][0] != '-') {
        value = argv[++i];
      }
      args.flags[key] = value;
      continue;
    }
    args.positional.push_back(arg);
  }

  if (args.positional.empty() || args.positional[0] == "help" || args.has("help")) {
    usage();
    return args.positional.empty() ? 2 : 0;
  }
  if (args.has("version")) {
    std::printf("coherence-fabric %s (%s)\n", coherence::version_string().c_str(),
                coherence::build_identification());
    return 0;
  }

  if (args.has("coordinator")) {
    Remote remote;
    if (!initialise_remote(remote, args)) return 1;
    const int code = run_remote(remote, args);
    (void)remote.client.close();
    return code;
  }

  Embedded embedded;
  if (!initialise_embedded(embedded, args)) return 1;
  return run_embedded(embedded, args);
}
