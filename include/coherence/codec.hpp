// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Canonical record codecs shared by persistence and the control plane.
//
// Every record carries an explicit record version. A decoder rejects an
// unknown version rather than guessing, and every decoder validates counts,
// lengths and enum values before allocating or mutating.
#ifndef COHERENCE_CODEC_HPP
#define COHERENCE_CODEC_HPP

#include <cstdint>

#include "coherence/decision.hpp"
#include "coherence/engine.hpp"
#include "coherence/evidence.hpp"
#include "coherence/export.hpp"
#include "coherence/model.hpp"
#include "coherence/policy.hpp"
#include "coherence/serialize.hpp"

namespace coherence {

inline constexpr std::uint16_t kRecordVersion = 1;

struct COHERENCE_API DecodeLimits {
  std::uint32_t max_text = 4096;
  std::uint64_t max_blob = 1ull << 28;
  std::uint64_t max_collection = 4'000'000;
  std::uint64_t max_object_length = 1ull << 40;
};

COHERENCE_API const DecodeLimits& default_decode_limits() noexcept;

// --- primitives ------------------------------------------------------------
COHERENCE_API void encode_fingerprint(ByteWriter& w, const ContentFingerprint& value);
COHERENCE_API bool decode_fingerprint(ByteReader& r, ContentFingerprint& out,
                                      const DecodeLimits& limits);

COHERENCE_API void encode_policy(ByteWriter& w, const CoherencePolicy& value);
COHERENCE_API bool decode_policy(ByteReader& r, CoherencePolicy& out, const DecodeLimits& limits);

COHERENCE_API void encode_evidence(ByteWriter& w, const EvidenceRecord& value);
COHERENCE_API bool decode_evidence(ByteReader& r, EvidenceRecord& out, const DecodeLimits& limits);

COHERENCE_API void encode_authority_context(ByteWriter& w, const AuthorityContext& value);
COHERENCE_API bool decode_authority_context(ByteReader& r, AuthorityContext& out,
                                            const DecodeLimits& limits);

// --- records ---------------------------------------------------------------
COHERENCE_API void encode_domain(ByteWriter& w, const DomainRecord& value);
COHERENCE_API bool decode_domain(ByteReader& r, DomainRecord& out, const DecodeLimits& limits);

COHERENCE_API void encode_participant(ByteWriter& w, const ParticipantRecord& value);
COHERENCE_API bool decode_participant(ByteReader& r, ParticipantRecord& out,
                                      const DecodeLimits& limits);

COHERENCE_API void encode_object(ByteWriter& w, const ObjectRecord& value);
COHERENCE_API bool decode_object(ByteReader& r, ObjectRecord& out, const DecodeLimits& limits);

COHERENCE_API void encode_region(ByteWriter& w, const RegionRecord& value);
COHERENCE_API bool decode_region(ByteReader& r, RegionRecord& out, const DecodeLimits& limits);

COHERENCE_API void encode_invalidation(ByteWriter& w, const InvalidationRecord& value);
COHERENCE_API bool decode_invalidation(ByteReader& r, InvalidationRecord& out,
                                       const DecodeLimits& limits);

COHERENCE_API void encode_sync(ByteWriter& w, const SyncRecord& value);
COHERENCE_API bool decode_sync(ByteReader& r, SyncRecord& out, const DecodeLimits& limits);

COHERENCE_API void encode_pending_publication(ByteWriter& w, const PendingPublication& value);
COHERENCE_API bool decode_pending_publication(ByteReader& r, PendingPublication& out,
                                              const DecodeLimits& limits);

// --- decisions -------------------------------------------------------------
COHERENCE_API void encode_read_decision(ByteWriter& w, const ReadDecision& value);
COHERENCE_API bool decode_read_decision(ByteReader& r, ReadDecision& out,
                                        const DecodeLimits& limits);

COHERENCE_API void encode_write_grant(ByteWriter& w, const WriteGrant& value);
COHERENCE_API bool decode_write_grant(ByteReader& r, WriteGrant& out, const DecodeLimits& limits);

COHERENCE_API void encode_publication(ByteWriter& w, const PublicationReceipt& value);
COHERENCE_API bool decode_publication(ByteReader& r, PublicationReceipt& out,
                                      const DecodeLimits& limits);

COHERENCE_API void encode_invalidation_outcome(ByteWriter& w, const InvalidationOutcome& value);
COHERENCE_API bool decode_invalidation_outcome(ByteReader& r, InvalidationOutcome& out,
                                               const DecodeLimits& limits);

COHERENCE_API void encode_sync_outcome(ByteWriter& w, const SyncOutcome& value);
COHERENCE_API bool decode_sync_outcome(ByteReader& r, SyncOutcome& out, const DecodeLimits& limits);

COHERENCE_API void encode_release_outcome(ByteWriter& w, const ReleaseOutcome& value);
COHERENCE_API bool decode_release_outcome(ByteReader& r, ReleaseOutcome& out,
                                          const DecodeLimits& limits);

// --- request payloads ------------------------------------------------------
COHERENCE_API void encode_read_request(ByteWriter& w, const ReadRequest& value);
COHERENCE_API bool decode_read_request(ByteReader& r, ReadRequest& out, const DecodeLimits& limits);

COHERENCE_API void encode_write_request(ByteWriter& w, const WriteRequest& value);
COHERENCE_API bool decode_write_request(ByteReader& r, WriteRequest& out,
                                        const DecodeLimits& limits);

COHERENCE_API void encode_dirty_request(ByteWriter& w, const DirtyRequest& value);
COHERENCE_API bool decode_dirty_request(ByteReader& r, DirtyRequest& out,
                                        const DecodeLimits& limits);

COHERENCE_API void encode_publish_request(ByteWriter& w, const PublishRequest& value);
COHERENCE_API bool decode_publish_request(ByteReader& r, PublishRequest& out,
                                          const DecodeLimits& limits);

COHERENCE_API void encode_release_request(ByteWriter& w, const ReleaseRequest& value);
COHERENCE_API bool decode_release_request(ByteReader& r, ReleaseRequest& out,
                                          const DecodeLimits& limits);

COHERENCE_API void encode_invalidation_request(ByteWriter& w, const InvalidationRequest& value);
COHERENCE_API bool decode_invalidation_request(ByteReader& r, InvalidationRequest& out,
                                               const DecodeLimits& limits);

COHERENCE_API void encode_invalidation_ack(ByteWriter& w, const InvalidationAck& value);
COHERENCE_API bool decode_invalidation_ack(ByteReader& r, InvalidationAck& out,
                                           const DecodeLimits& limits);

COHERENCE_API void encode_sync_request(ByteWriter& w, const SyncRequest& value);
COHERENCE_API bool decode_sync_request(ByteReader& r, SyncRequest& out, const DecodeLimits& limits);

COHERENCE_API void encode_sync_complete(ByteWriter& w, const SyncCompleteRequest& value);
COHERENCE_API bool decode_sync_complete(ByteReader& r, SyncCompleteRequest& out,
                                        const DecodeLimits& limits);

COHERENCE_API void encode_sync_fail(ByteWriter& w, const SyncFailRequest& value);
COHERENCE_API bool decode_sync_fail(ByteReader& r, SyncFailRequest& out,
                                    const DecodeLimits& limits);

COHERENCE_API void encode_revalidate_request(ByteWriter& w, const RevalidateRequest& value);
COHERENCE_API bool decode_revalidate_request(ByteReader& r, RevalidateRequest& out,
                                             const DecodeLimits& limits);

COHERENCE_API void encode_region_registration(ByteWriter& w, const RegionRegistration& value);
COHERENCE_API bool decode_region_registration(ByteReader& r, RegionRegistration& out,
                                              const DecodeLimits& limits);

} // namespace coherence

#endif // COHERENCE_CODEC_HPP
