// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// A shared mapping observed by two independent handles: the same physical pages
// seen through two mappings, with real bytes and real verification.
#include "example_support.hpp"

using namespace coherence;

int main() {
  example::Session session(strict_policy("example-strict"));
  const std::string segment = "cf-example-" + generate_request_id().to_string();
  auto created = session.store.add_shared_region("primary", segment, 4096, true);
  if (!created.has_value()) {
    example::say("shared mapping unavailable: " + created.status().to_string());
    return 1;
  }
  auto observer = adapters::SharedSegment::open(segment, 4096, false);
  if (!observer.has_value()) {
    example::say("second mapping unavailable: " + observer.status().to_string());
    return 1;
  }
  const RegionRecord region = session.add_region("shared", 0, 4096, MemoryDomain::HostShared);
  (void)region;
  (void)session.store.poke(created.value(), 128, 0xC3, 64);
  bool identical = true;
  for (std::size_t i = 0; i < 64; ++i) {
    if (std::to_integer<std::uint8_t>(observer.value().span()[128 + i]) != 0xC3u) identical = false;
  }
  example::say(std::string("two mappings observe identical bytes: ") +
               (identical ? "true" : "false"));
  example::say("region memory domain: " + std::string(to_token(MemoryDomain::HostShared)));
  return identical ? 0 : 1;
}
