// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// Stale generations never regain authority: an old boot identity, an old
// ownership generation and an old coordinator epoch are all refused explicitly.
#include "example_support.hpp"

using namespace coherence;

int main() {
  example::Session session(strict_policy("example-strict"));
  const RegionRecord replica = session.add_region("replica", 0, 4096);
  session.revalidate(replica.id, VersionId::from_value(1));
  WriteRequest write;
  write.context = session.context();
  write.region = replica.id;
  write.region_generation = replica.generation;
  const auto grant = session.engine->acquire_write(write);
  const OwnershipGeneration stale = grant.value().context.ownership_generation;

  ReleaseRequest release;
  release.context = session.context();
  release.ownership_generation = stale;
  release.release_write_authority = true;
  (void)session.engine->release(release);

  PublishRequest publish;
  publish.context = session.context();
  publish.ownership_generation = stale;
  publish.region = replica.id;
  publish.region_generation = replica.generation;
  publish.expected_base_version = VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = session.fingerprint(replica.id);
  const auto rejected = session.engine->publish(publish);
  example::say(std::string("publication with a released ownership generation: ") +
               (rejected.has_value() ? std::string(status_code_name(rejected.value().reason))
                                     : rejected.status().to_string()));

  ReadRequest read;
  read.context = session.context();
  read.context.boot = ParticipantBootId::from_value(UInt128{1, 1});
  const auto stale_boot = session.engine->acquire_read(read);
  example::say(std::string("read from an unknown boot identity: ") +
               std::string(status_code_name(stale_boot.value().reason)));

  read.context = session.context();
  read.context.epoch = CoordinatorEpoch::from_value(999);
  const auto stale_epoch = session.engine->acquire_read(read);
  example::say(std::string("read under a stale coordinator epoch: ") +
               (stale_epoch.has_value() ? std::string(status_code_name(stale_epoch.value().reason))
                                        : stale_epoch.status().to_string()));
  return 0;
}
