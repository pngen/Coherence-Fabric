// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// One logical object, two real replicas, one exclusive writer at a time.
#include "example_support.hpp"

using namespace coherence;

int main() {
  example::Session session(strict_policy("example-strict"));
  const RegionRecord alpha = session.add_region("alpha", 0, 4096);
  const RegionRecord beta = session.add_region("beta", 0, 4096);
  (void)beta;

  example::say("registered replicas: " + alpha.id.to_string());

  // A replica is not current merely because it exists.
  example::say(std::string("initial coherence state: ") + std::string(to_token(alpha.state)));

  const RegionRecord current = session.revalidate(alpha.id, VersionId::from_value(1));
  example::say(std::string("after a real byte observation: ") +
               std::string(to_token(current.state)));

  WriteRequest write;
  write.context = session.context();
  write.region = alpha.id;
  write.region_generation = alpha.generation;
  const auto grant = session.engine->acquire_write(write);
  example::say(std::string("write authority granted: ") +
               (grant.value().granted ? "true" : "false"));
  example::say("ownership generation: " + grant.value().context.ownership_generation.to_string());

  PublishRequest publish;
  publish.context = session.context();
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = alpha.id;
  publish.region_generation = alpha.generation;
  publish.expected_base_version = VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = session.fingerprint(alpha.id);
  const auto receipt = session.engine->publish(publish);
  example::say("published version: " + receipt.value().version.to_string());
  example::say(std::string("durable: ") + (receipt.value().durable ? "true" : "false") +
               " (no durable store is attached in this example)");

  const AuditReport audit = session.engine->audit();
  example::say(std::string("invariant audit: ") + (audit.clean ? "clean" : "violations"));
  return audit.clean ? 0 : 1;
}
