// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// A coordinator restart advances the authority epoch and downgrades every
// replica. Persisted metadata is provenance, never proof of currentness.
#include "coherence/persistence.hpp"

#include "example_support.hpp"

using namespace coherence;

int main() {
  auto store = std::make_shared<MemoryDurableStore>();
  RegionId replica_id;
  VersionId published;
  {
    example::Session session(strict_policy("example-strict"), 4096, true, store);
    const RegionRecord replica = session.add_region("replica", 0, 4096);
    replica_id = replica.id;
    session.revalidate(replica.id, VersionId::from_value(1));
    WriteRequest write;
    write.context = session.context();
    write.region = replica.id;
    write.region_generation = replica.generation;
    const auto grant = session.engine->acquire_write(write);
    PublishRequest publish;
    publish.context = session.context();
    publish.ownership_generation = grant.value().context.ownership_generation;
    publish.region = replica.id;
    publish.region_generation = replica.generation;
    publish.expected_base_version = VersionId::nil();
    publish.allow_without_dirty = true;
    publish.content = session.fingerprint(replica.id);
    published = session.engine->publish(publish).value().version;
    example::say("published version " + published.to_string() +
                 " and now simulating a coordinator restart");
  }
  EngineConfig config;
  CoherenceEngine restarted(config);
  const auto recovered = restarted.attach_store(store);
  if (!recovered.has_value()) {
    example::say("recovery failed: " + recovered.status().to_string());
    return 1;
  }
  example::say("epoch advanced from " + recovered.value().previous_epoch.to_string() + " to " +
               recovered.value().new_epoch.to_string());
  example::say("regions downgraded: " + std::to_string(recovered.value().regions_downgraded));
  const auto region = restarted.get_region(replica_id);
  example::say(std::string("restored replica state: ") + std::string(to_token(region.value().state)));
  const auto audit = restarted.audit();
  example::say(std::string("invariant audit: ") + (audit.clean ? "clean" : "violations"));
  return region.value().state == CoherenceState::RevalidationRequired && audit.clean ? 0 : 1;
}
