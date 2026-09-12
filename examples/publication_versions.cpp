// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// Versions advance only through publication. A completed write is not a
// published version, and a publication is not implicitly durable.
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
  PublishRequest publish;
  publish.context = session.context();
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = replica.id;
  publish.region_generation = replica.generation;
  publish.expected_base_version = VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = session.fingerprint(replica.id);
  const auto first = session.engine->publish(publish);
  example::say("version 1 published as " + first.value().publication.to_string());

  // Mutating real bytes without publishing does not move the authoritative
  // version.
  (void)session.store.poke(replica.id, 0, 0x7E, 32);
  const auto object_after_mutation = session.engine->get_object(session.object);
  example::say("authoritative version after a silent mutation: " +
               object_after_mutation.value().authoritative_version.to_string());

  DirtyRequest dirty;
  dirty.context = session.context();
  dirty.region = replica.id;
  dirty.region_generation = replica.generation;
  dirty.ownership_generation = grant.value().context.ownership_generation;
  dirty.base_version = first.value().version;
  (void)session.engine->mark_dirty(dirty);
  PublishRequest second = publish;
  second.context = session.context();
  second.expected_base_version = first.value().version;
  second.allow_without_dirty = false;
  second.content = session.fingerprint(replica.id);
  const auto published = session.engine->publish(second);
  example::say("version 2 published as " + published.value().version.to_string());
  return published.value().version.value() == 2 ? 0 : 1;
}
