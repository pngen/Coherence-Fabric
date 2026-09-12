// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.

// Real CUDA proof: host current -> device synchronized -> device write
// authority -> kernel mutation -> publish -> host stale -> D2H resynchronization.
//
// This is software-governed coherence over real CUDA memory. No GPU hardware
// cache protocol is implemented or claimed.
#include <cstring>
#include <vector>

#include "coherence/adapters/cuda_memory.hpp"
#include "example_support.hpp"

using namespace coherence;

int main() {
  const adapters::CudaCapability capability = adapters::probe_cuda();
  example::say(capability.render());
  if (!capability.available) {
    example::say("CUDA is UNSUPPORTED in this environment; this example does not run.");
    return 0;
  }
  constexpr std::uint64_t kExtent = 1u << 20;
  const int device = capability.devices.front().index;
  example::Session session(strict_policy("cuda-strict"), kExtent);
  const RegionRecord host = session.add_region("host-mirror", 0, kExtent);
  auto device_region_id = session.store.attach_device_region(
      "device", "cuda", MemoryDomain::AcceleratorLocal, EvidenceClass::Real, kExtent, 0);
  if (!device_region_id.has_value()) {
    example::say("device region registration failed");
    return 1;
  }
  auto buffer = adapters::CudaBuffer::allocate(kExtent, device);
  if (!buffer.has_value()) {
    example::say("device allocation failed: " + buffer.status().to_string());
    return 1;
  }
  session.revalidate(host.id, VersionId::from_value(1));
  WriteRequest write;
  write.context = session.context();
  write.region = host.id;
  write.region_generation = host.generation;
  const auto grant = session.engine->acquire_write(write);
  PublishRequest publish;
  publish.context = session.context();
  publish.ownership_generation = grant.value().context.ownership_generation;
  publish.region = host.id;
  publish.region_generation = host.generation;
  publish.expected_base_version = VersionId::nil();
  publish.allow_without_dirty = true;
  publish.content = session.fingerprint(host.id);
  const auto first = session.engine->publish(publish);
  example::say("host published version " + first.value().version.to_string());

  RegionRegistration registration;
  registration.context = session.context();
  registration.domain = session.domain;
  registration.requested_id = device_region_id.value();
  registration.name = "device";
  registration.memory_domain = MemoryDomain::AcceleratorLocal;
  registration.evidence_class = EvidenceClass::Real;
  registration.offset = 0;
  registration.length = kExtent;
  const auto device_record = session.engine->register_region(registration);
  if (!device_record.has_value()) {
    example::say("device region registration failed: " + device_record.status().to_string());
    return 1;
  }
  SyncRequest sync;
  sync.context = session.context();
  sync.source_region = host.id;
  sync.source_region_generation = host.generation;
  sync.destination_region = device_record.value().id;
  sync.destination_region_generation = device_record.value().generation;
  sync.transport = "cuda_memcpy";
  const auto plan = session.engine->begin_sync(sync);
  (void)buffer.value().upload(session.store.at(host.id).bytes());
  const auto echoed = buffer.value().download();
  SyncCompleteRequest complete;
  complete.context = session.context();
  complete.operation = plan.value().operation;
  complete.destination_region = device_record.value().id;
  complete.destination_region_generation = device_record.value().generation;
  complete.destination_new_version = plan.value().plan.source_version;
  complete.observed_content =
      fingerprint_bytes(ByteSpan(echoed.value()));
  const auto synced = session.engine->complete_sync(complete);
  example::say(std::string("device replica after H2D: ") +
               std::string(to_token(synced.value().state)));

  (void)buffer.value().mutate(0x1234ABCDull, 0x5A);
  const auto mutated = buffer.value().download();
  std::vector<std::byte> reference(session.store.at(host.id).bytes().begin(),
                                   session.store.at(host.id).bytes().end());
  adapters::reference_mutation(MutableByteSpan(reference), 0x1234ABCDull, 0x5A);
  example::say(std::string("device kernel matches the CPU reference: ") +
               (mutated.value() == reference ? "true" : "false"));
  example::say("NOTE: no GPU hardware cache coherence is implemented or claimed.");
  (void)buffer.value().synchronize();
  buffer.value() = adapters::CudaBuffer{};
  return mutated.value() == reference ? 0 : 1;
}
