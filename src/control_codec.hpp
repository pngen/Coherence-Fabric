// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Request and response payload codecs shared by the control client and the
// coordinator. Internal; not installed.
#ifndef COHERENCE_SRC_CONTROL_CODEC_HPP
#define COHERENCE_SRC_CONTROL_CODEC_HPP

#include <string>
#include <utility>

#include "coherence/codec.hpp"
#include "coherence/protocol.hpp"

namespace coherence {
namespace control {

inline void enc_text(ByteWriter& w, std::string_view value) { w.text(value); }

inline bool dec_text(ByteReader& r, std::string& out, const DecodeLimits& limits) {
  return r.text(out, limits.max_text);
}

// --- create domain ---------------------------------------------------------
inline void encode_create_domain(ByteWriter& w, std::string_view name) { w.text(name); }
inline bool decode_create_domain(ByteReader& r, std::string& name, const DecodeLimits& limits) {
  return r.text(name, limits.max_text);
}

// --- register object -------------------------------------------------------
inline void encode_register_object(ByteWriter& w, CoherenceDomainId domain, std::string_view name,
                                   std::uint64_t length, const CoherencePolicy& policy) {
  w.strong_id(domain);
  w.text(name);
  w.u64(length);
  encode_policy(w, policy);
}

inline bool decode_register_object(ByteReader& r, CoherenceDomainId& domain, std::string& name,
                                   std::uint64_t& length, CoherencePolicy& policy,
                                   const DecodeLimits& limits) {
  if (!r.strong_id(domain)) return false;
  if (!r.text(name, limits.max_text)) return false;
  if (!r.u64(length)) return false;
  if (!decode_policy(r, policy, limits)) return false;
  return true;
}

// --- set policy ------------------------------------------------------------
inline void encode_set_policy(ByteWriter& w, ObjectId object, ObjectGeneration generation,
                              PolicyGeneration expected, const CoherencePolicy& policy) {
  w.strong_id(object);
  w.strong_id(generation);
  w.strong_id(expected);
  encode_policy(w, policy);
}

inline bool decode_set_policy(ByteReader& r, ObjectId& object, ObjectGeneration& generation,
                              PolicyGeneration& expected, CoherencePolicy& policy,
                              const DecodeLimits& limits) {
  if (!r.strong_id(object)) return false;
  if (!r.strong_id(generation)) return false;
  if (!r.strong_id(expected)) return false;
  if (!decode_policy(r, policy, limits)) return false;
  return true;
}

// --- fence / retire --------------------------------------------------------
inline void encode_fence_participant(ByteWriter& w, ParticipantId id, std::string_view reason) {
  w.strong_id(id);
  w.text(reason);
}

inline bool decode_fence_participant(ByteReader& r, ParticipantId& id, std::string& reason,
                                     const DecodeLimits& limits) {
  if (!r.strong_id(id)) return false;
  return r.text(reason, limits.max_text);
}

inline void encode_retire(ByteWriter& w, std::uint64_t id, std::uint64_t generation) {
  w.u64(id);
  w.u64(generation);
}

inline bool decode_retire(ByteReader& r, std::uint64_t& id, std::uint64_t& generation) {
  if (!r.u64(id)) return false;
  return r.u64(generation);
}

// --- snapshot options ------------------------------------------------------
inline void encode_snapshot_options(ByteWriter& w, const SnapshotOptions& options) {
  w.u64(options.object_filter.value());
  w.u64(options.max_objects);
  w.boolean(options.include_regions);
  w.boolean(options.include_participants);
  w.boolean(options.include_invalidations);
  w.boolean(options.include_syncs);
}

inline bool decode_snapshot_options(ByteReader& r, SnapshotOptions& options) {
  std::uint64_t filter = 0;
  if (!r.u64(filter)) return false;
  options.object_filter = ObjectId::from_value(filter);
  if (!r.u64(options.max_objects)) return false;
  if (!r.boolean(options.include_regions)) return false;
  if (!r.boolean(options.include_participants)) return false;
  if (!r.boolean(options.include_invalidations)) return false;
  if (!r.boolean(options.include_syncs)) return false;
  return true;
}

// --- object lookup ---------------------------------------------------------
inline void encode_object_ref(ByteWriter& w, ObjectId id) { w.strong_id(id); }
inline bool decode_object_ref(ByteReader& r, ObjectId& id) { return r.strong_id(id); }

inline void encode_region_ref(ByteWriter& w, RegionId id) { w.strong_id(id); }
inline bool decode_region_ref(ByteReader& r, RegionId& id) { return r.strong_id(id); }

// --- rendered text ---------------------------------------------------------
inline void encode_text_block(ByteWriter& w, std::string_view text) { w.text(text); }
inline bool decode_text_block(ByteReader& r, std::string& text, const DecodeLimits& limits) {
  return r.text(text, limits.max_text == 0 ? 4096u : limits.max_text);
}

} // namespace control
} // namespace coherence

#endif // COHERENCE_SRC_CONTROL_CODEC_HPP
