// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// The agent: a participant process that exposes real memory regions to the
// coherence domain and acts on coordinator decisions.
#ifndef COHERENCE_AGENT_HPP
#define COHERENCE_AGENT_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coherence/adapters/region_store.hpp"
#include "coherence/client.hpp"
#include "coherence/export.hpp"
#include "coherence/protocol.hpp"
#include "coherence/status.hpp"

namespace coherence {

struct COHERENCE_API RegionSpec {
  std::string name;
  MemoryDomain memory_domain = MemoryDomain::HostPageable;
  adapters::RegionBacking backing = adapters::RegionBacking::HostHeap;
  std::string segment_name;
  bool create_segment = false;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::uint64_t seed = 0;
  bool declared_writable = true;
};

struct COHERENCE_API AgentOptions {
  ClientOptions client;
  std::string domain_name = "default";
  std::string object_name = "object";
  std::uint64_t object_length = 0;
  ConsistencyModel consistency = ConsistencyModel::Strict;
  std::uint64_t stale_read_bound = 0;
  StaleReadPolicy stale_read_policy = StaleReadPolicy::Never;
  std::vector<RegionSpec> regions;

  /// Optional staging segment shared between participants. It is the real byte
  /// path used by the cross-process synchronization proof: the publisher writes
  /// its authoritative bytes here and the destination copies them out of it.
  std::string staging_segment;
  std::uint64_t staging_length = 0;
  bool staging_create = false;

  /// When true the agent keeps its session open after running the script. Used
  /// by the multiprocess proof so the process can be killed mid-lifecycle.
  bool stay_open = false;
  std::vector<std::string> script;
};

/// Line-oriented command surface. Each command prints stable machine-readable
/// lines so the multiprocess proof can assert on exact outcomes.
class COHERENCE_API Agent {
 public:
  Agent();
  ~Agent();
  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;

  Status configure(AgentOptions options);

  /// Connect, register the participant, resolve or create the domain and
  /// object, register every configured region.
  Status join();

  /// Execute one script command, appending its output lines to the sink.
  Status execute(std::string_view command, std::vector<std::string>& sink);

  Status run_script(std::vector<std::string>& sink);

  /// Release write authority and read leases, then close the session cleanly.
  /// Idempotent.
  Status leave();

  [[nodiscard]] ControlClient& client() noexcept { return client_; }
  [[nodiscard]] adapters::RegionStore& store() noexcept { return store_; }
  [[nodiscard]] const AgentOptions& options() const noexcept { return options_; }
  [[nodiscard]] ObjectId object() const noexcept { return object_; }
  [[nodiscard]] ObjectGeneration object_generation() const noexcept { return object_generation_; }
  [[nodiscard]] VersionId authoritative_version() const noexcept { return authoritative_version_; }
  [[nodiscard]] OwnershipGeneration ownership() const noexcept { return ownership_; }
  [[nodiscard]] PolicyGeneration policy_generation() const noexcept { return policy_generation_; }
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] RegionId region_id(std::string_view name) const;
  [[nodiscard]] RegionGeneration region_generation(RegionId id) const;
  [[nodiscard]] VersionId region_version(RegionId id) const;
  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
  [[nodiscard]] bool joined() const noexcept { return joined_; }

 private:
  Status ensure_object();
  Status refresh_object();
  Status register_regions();
  AuthorityContext make_context() const;
  Status send_region_registration(const RegionSpec& spec, RegionId id, std::string& rendered);
  RegionId next_region_identity();
  Status command_ack_pending(std::vector<std::string>& sink);
  Status command_read(const std::vector<std::string>& args, std::vector<std::string>& sink);
  Status command_write(const std::vector<std::string>& args, std::vector<std::string>& sink);
  Status command_publish(const std::vector<std::string>& args, std::vector<std::string>& sink);
  Status command_sync_begin(const std::vector<std::string>& args, std::vector<std::string>& sink);
  Status command_sync_complete(const std::vector<std::string>& args,
                               std::vector<std::string>& sink);
  Status command_invalidate(const std::vector<std::string>& args,
                            std::vector<std::string>& sink);

  AgentOptions options_;
  ControlClient client_;
  adapters::RegionStore store_;
  std::map<std::string, RegionId> region_ids_;
  std::map<RegionId, RegionGeneration> region_generations_;
  std::map<RegionId, VersionId> region_versions_;
  std::map<SyncOperationId, SyncRecord> sync_plans_;
  RegionId last_region_;
  LeaseId last_lease_;
  ObjectId object_;
  CoherenceDomainId domain_;
  ObjectGeneration object_generation_;
  VersionId authoritative_version_;
  OwnershipGeneration ownership_;
  PolicyGeneration policy_generation_;
  CoordinatorEpoch epoch_;
  std::shared_ptr<adapters::SharedSegment> staging_;
  std::string last_error_;
  bool joined_ = false;
  bool write_held_ = false;
  std::uint64_t next_region_fallback_ = 1;
};

/// Split a command line into whitespace-separated tokens.
COHERENCE_API std::vector<std::string> split_command(std::string_view line);

} // namespace coherence

#endif // COHERENCE_AGENT_HPP
