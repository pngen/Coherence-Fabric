// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// The same stale replica produces different, explicitly reported outcomes under
// strict and eventual policies. Neither ever reports a stale replica as current.
#include "example_support.hpp"

using namespace coherence;

/// Build a situation where the reader replica is genuinely STALE: it was
/// synchronized at version 1 and the writer has since published version 2.
ReadOutcome read_with(const CoherencePolicy& policy, MemoryDomain domain_tag) {
  example::Session session(policy);
  const RegionRecord writer = session.add_region("writer", 0, 4096);
  const RegionRecord reader = session.add_region("reader", 0, 4096, domain_tag);
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
  const auto first = session.engine->publish(publish);

  // Bring the reader to current at version 1 with a verified byte copy.
  SyncRequest sync;
  sync.context = session.context();
  sync.source_region = writer.id;
  sync.source_region_generation = writer.generation;
  sync.destination_region = reader.id;
  sync.destination_region_generation = reader.generation;
  sync.transport = "in_process_byte_copy";
  const auto plan = session.engine->begin_sync(sync);
  (void)session.store.copy_and_verify(writer.id, reader.id);
  SyncCompleteRequest complete;
  complete.context = session.context();
  complete.operation = plan.value().operation;
  complete.destination_region = reader.id;
  complete.destination_region_generation = reader.generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content = session.fingerprint(reader.id);
  (void)session.engine->complete_sync(complete);

  // Now publish version 2 from the writer. The reader is left at version 1 and
  // is therefore genuinely stale.
  WriteRequest again;
  again.context = session.context();
  again.region = writer.id;
  again.region_generation = writer.generation;
  const auto second_grant = session.engine->acquire_write(again);
  DirtyRequest dirty;
  dirty.context = session.context();
  dirty.region = writer.id;
  dirty.region_generation = writer.generation;
  dirty.ownership_generation = second_grant.value().context.ownership_generation;
  dirty.base_version = first.value().version;
  (void)session.engine->mark_dirty(dirty);
  (void)session.store.poke(writer.id, 0, 0x33, 16);
  PublishRequest second = publish;
  second.context = session.context();
  second.expected_base_version = first.value().version;
  second.allow_without_dirty = false;
  second.content = session.fingerprint(writer.id);
  (void)session.engine->publish(second);

  ReadRequest read;
  read.context = session.context();
  read.region = reader.id;
  read.require_current = true;
  const auto decision = session.engine->acquire_read(read);
  example::say("  replica state: " + std::string(to_token(decision.value().region_state)));
  example::say("  replica version: " + decision.value().region_version.to_string() +
               " authoritative version: " + decision.value().authoritative_version.to_string());
  example::say("  rationale: " + decision.value().rationale);
  return decision.value().outcome;
}

int main() {
  example::say("strict policy on a never-synchronized replica:");
  const ReadOutcome strict_outcome = read_with(strict_policy("strict"), MemoryDomain::HostPageable);
  example::say(std::string("strict outcome: ") + std::string(to_token(strict_outcome)));

  example::say("eventual policy on the same situation:");
  const ReadOutcome relaxed =
      read_with(eventual_policy(8, "eventual"), MemoryDomain::HostPageable);
  example::say(std::string("eventual outcome: ") + std::string(to_token(relaxed)));

  return strict_outcome != ReadOutcome::ReadCurrent ? 0 : 1;
}
