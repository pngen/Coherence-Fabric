// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
#include "coherence/version.hpp"

#define COHERENCE_STRINGIFY_IMPL(x) #x
#define COHERENCE_STRINGIFY(x) COHERENCE_STRINGIFY_IMPL(x)

#if defined(_MSC_VER)
#  define COHERENCE_COMPILER_TAG "msvc-" COHERENCE_STRINGIFY(_MSC_VER)
#elif defined(__clang__)
#  define COHERENCE_COMPILER_TAG "clang-" COHERENCE_STRINGIFY(__clang_major__)
#elif defined(__GNUC__)
#  define COHERENCE_COMPILER_TAG "gcc-" COHERENCE_STRINGIFY(__GNUC__)
#else
#  define COHERENCE_COMPILER_TAG "unknown-compiler"
#endif

#define COHERENCE_VERSION_LITERAL          \
  COHERENCE_STRINGIFY(COHERENCE_FABRIC_VERSION_MAJOR) "." \
  COHERENCE_STRINGIFY(COHERENCE_FABRIC_VERSION_MINOR) "." \
  COHERENCE_STRINGIFY(COHERENCE_FABRIC_VERSION_PATCH)

#if defined(NDEBUG)
#  define COHERENCE_BUILD_TAG " release"
#else
#  define COHERENCE_BUILD_TAG " debug"
#endif

namespace coherence {
namespace {

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kPersistenceSchema = 1;

} // namespace

Version library_version() noexcept { return Version{}; }

std::string version_string() { return std::string{COHERENCE_VERSION_LITERAL}; }

std::uint32_t protocol_version() noexcept { return kProtocolVersion; }

std::uint32_t persistence_schema() noexcept { return kPersistenceSchema; }

const char* build_identification() noexcept {
  return "coherence-fabric " COHERENCE_VERSION_LITERAL " [" COHERENCE_COMPILER_TAG
         COHERENCE_BUILD_TAG "]";
}

} // namespace coherence
