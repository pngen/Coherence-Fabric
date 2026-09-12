// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
// Shared scaffolding for the runnable examples.
#ifndef COHERENCE_EXAMPLE_SUPPORT_HPP
#define COHERENCE_EXAMPLE_SUPPORT_HPP

#include <cstdio>
#include <memory>
#include <string>

#include "coherence/adapters/region_store.hpp"
#include "coherence/engine.hpp"

namespace example {

inline void say(const std::string& text) {
  std::printf("%s\n", text.c_str());
  std::fflush(stdout);
}

struct Session {
  coherence::EngineConfig config;
  std::unique_ptr<coherence::CoherenceEngine> engine;
  coherence::adapters::RegionStore store;
  coherence::CoherenceDomainId domain;
  coherence::ObjectId object;
  coherence::ParticipantRecord participant;

  explicit Session(const coherence::CoherencePolicy& policy, std::uint64_t length = 4096,
                   bool durable = false,
                   const std::shared_ptr<coherence::DurableStore>& store = nullptr) {
    config.enable_durability = durable || store != nullptr;
    engine = std::make_unique<coherence::CoherenceEngine>(config);
    // Durable state must be attached before anything is created, because
    // attaching it replaces the in-memory image with the durable one.
    if (store != nullptr) {
      auto attached = engine->attach_store(store);
      if (!attached.has_value()) {
        say("durable store could not be attached: " + attached.status().to_string());
      }
    }
    domain = engine->create_domain("example").value().id;
    object = engine->register_object(domain, "object", length, policy).value().id;
    participant = engine->register_participant("example", coherence::generate_participant_boot_id(),
                                               engine->epoch(), "example-node")
                      .value();
  }

  coherence::AuthorityContext context() {
    coherence::AuthorityContext ctx;
    ctx.epoch = engine->epoch();
    ctx.participant = participant.id;
    ctx.boot = participant.boot;
    ctx.object = object;
    const auto record = engine->get_object(object).value();
    ctx.object_generation = record.generation;
    ctx.policy_generation = record.policy_generation;
    ctx.request = coherence::generate_request_id();
    return ctx;
  }

  coherence::RegionRecord add_region(const std::string& name, std::uint64_t offset,
                                     std::uint64_t length,
                                     coherence::MemoryDomain domain_tag =
                                         coherence::MemoryDomain::HostPageable) {
    coherence::RegionId requested = coherence::RegionId::nil();
    auto created = domain_tag == coherence::MemoryDomain::CxlClass
                       ? store.add_synthetic_cxl_region(name, length, 1)
                       : store.add_host_region(name, length, domain_tag, 1, requested);
    store.at(created.value()).evidence_class =
        domain_tag == coherence::MemoryDomain::CxlClass ? coherence::EvidenceClass::Synthetic
                                                        : coherence::EvidenceClass::Real;
    coherence::RegionRegistration registration;
    registration.context = context();
    registration.domain = domain;
    registration.requested_id = created.value();
    registration.name = name;
    registration.memory_domain = domain_tag;
    registration.evidence_class = store.at(created.value()).evidence_class;
    registration.offset = offset;
    registration.length = length;
    registration.address_hint = store.at(created.value()).address_hint;
    registration.content = coherence::fingerprint_bytes(store.at(created.value()).bytes());
    return engine->register_region(registration).value();
  }

  coherence::ContentFingerprint fingerprint(coherence::RegionId id) {
    return coherence::fingerprint_bytes(store.at(id).bytes());
  }

  coherence::RegionRecord revalidate(coherence::RegionId id, coherence::VersionId version) {
    coherence::RevalidateRequest request;
    request.context = context();
    request.region = id;
    request.region_generation = engine->get_region(id).value().generation;
    request.observed_version = version;
    request.content = fingerprint(id);
    auto updated = engine->revalidate_region(request);
    if (!updated.has_value()) {
      say("revalidation failed: " + updated.status().to_string());
      return {};
    }
    return updated.value();
  }
};

} // namespace example

#endif // COHERENCE_EXAMPLE_SUPPORT_HPP
