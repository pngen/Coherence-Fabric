// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// A host replica is invalidated when another replica publishes, and is brought
// back to current only by a synchronization whose bytes are verified.
#include "example_support.hpp"

using namespace coherence;

int main() {
  example::Session session(strict_policy("example-strict"));
  const RegionRecord writer = session.add_region("writer", 0, 4096);
  const RegionRecord reader = session.add_region("reader", 0, 4096);
  session.revalidate(writer.id, VersionId::from_value(1));

  WriteRequest write;
  write.context = session.context();
  write.region = writer.id;
  write.region_generation = writer.generation;
  const auto grant = session.engine->acquire_write(write);
  PublishRequest publish;
  publish.context = session.context();
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = writer.id;
  publish.region_generation = writer.generation;
  publish.expected_base_version = VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = session.fingerprint(writer.id);
  (void)session.engine->publish(publish);

  ReadRequest read;
  read.context = session.context();
  read.region = reader.id;
  read.require_current = true;
  const auto before = session.engine->acquire_read(read);
  example::say(std::string("read before synchronization: ") +
               std::string(to_token(before.value().outcome)));

  SyncRequest sync;
  sync.context = session.context();
  sync.source_region = writer.id;
  sync.source_region_generation = writer.generation;
  sync.destination_region = reader.id;
  sync.destination_region_generation = reader.generation;
  sync.transport = "in_process_byte_copy";
  const auto plan = session.engine->begin_sync(sync);
  const Status copied = session.store.copy_and_verify(writer.id, reader.id);
  example::say(std::string("real byte copy verified: ") + (copied.ok() ? "true" : "false"));

  SyncCompleteRequest complete;
  complete.context = session.context();
  complete.operation = plan.value().operation;
  complete.destination_region = reader.id;
  complete.destination_region_generation = reader.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = session.fingerprint(reader.id);
  const auto outcome = session.engine->complete_sync(complete);
  example::say(std::string("synchronization: ") + std::string(to_token(outcome.value().state)));

  const auto after = session.engine->acquire_read(read);
  example::say(std::string("read after synchronization: ") +
               std::string(to_token(after.value().outcome)));
  return after.value().outcome == ReadOutcome::ReadCurrent ? 0 : 1;
}
