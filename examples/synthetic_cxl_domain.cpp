// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// A CXL-class memory domain with no physical CXL hardware behind it. The
// runtime exercises domain identity, locality metadata and ownership
// transitions, and labels the evidence SYNTHETIC throughout.
#include "example_support.hpp"

using namespace coherence;

int main() {
  example::Session session(strict_policy("example-strict"));
  const RegionRecord cxl = session.add_region("cxl-pool", 0, 4096, MemoryDomain::CxlClass);
  example::say("region memory domain: " + std::string(to_token(cxl.memory_domain)));
  example::say("region evidence class: " + std::string(to_token(cxl.evidence_class)));
  const RegionRecord current = session.revalidate(cxl.id, VersionId::from_value(1));
  example::say(std::string("state after revalidation: ") + std::string(to_token(current.state)));
  example::say("NOTE: no physical CXL device, switch or fabric-attached memory is present. "
               "CXL.cache and CXL.mem behaviour is UNSUPPORTED and is not claimed.");
  return current.evidence_class == EvidenceClass::Synthetic ? 0 : 1;
}
