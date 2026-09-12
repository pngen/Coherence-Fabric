// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Sanitizer instrumentation probe.
//
// Without arguments it links against the runtime and exits successfully, which
// proves the library itself is built and linked under the sanitizer. With
// --trigger it deliberately performs an out-of-bounds write, which a genuine
// AddressSanitizer run must detect and report. An uninstrumented build would
// exit successfully, which is exactly what the test asserts against.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "coherence/engine.hpp"
#include "coherence/policy.hpp"
#include "coherence/version.hpp"

int main(int argc, char** argv) {
  coherence::CoherenceEngine engine;
  const auto domain = engine.create_domain("asan-probe");
  if (!domain.has_value()) {
    std::fprintf(stderr, "probe setup failed: %s\n", domain.status().to_string().c_str());
    return 2;
  }
  std::printf("instrumented=%s\n", coherence::build_identification());
  std::fflush(stdout);

  if (argc > 1 && std::string(argv[1]) == "--trigger") {
    // Deliberate heap overflow. A sanitized build aborts here and reports it.
    std::vector<std::byte> buffer(16);
    volatile std::byte* cursor = buffer.data();
    for (int i = 0; i < 64; ++i) {
      cursor[i] = static_cast<std::byte>(i);
    }
    std::printf("no-sanitizer-report\n");
    std::fflush(stdout);
    return 0;
  }
  return 0;
}
