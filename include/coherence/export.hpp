// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Shared-library visibility / linkage decoration.
#ifndef COHERENCE_EXPORT_HPP
#define COHERENCE_EXPORT_HPP

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(COHERENCE_FABRIC_BUILD_SHARED)
#    define COHERENCE_API __declspec(dllexport)
#  elif defined(COHERENCE_FABRIC_USE_SHARED)
#    define COHERENCE_API __declspec(dllimport)
#  else
#    define COHERENCE_API
#  endif
#  define COHERENCE_LOCAL
#else
#  if defined(COHERENCE_FABRIC_BUILD_SHARED) && (defined(__GNUC__) || defined(__clang__))
#    define COHERENCE_API __attribute__((visibility("default")))
#    define COHERENCE_LOCAL __attribute__((visibility("hidden")))
#  else
#    define COHERENCE_API
#    define COHERENCE_LOCAL
#  endif
#endif

#endif // COHERENCE_EXPORT_HPP
