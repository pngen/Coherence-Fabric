# Coherence Fabric

**Open-source, vendor-neutral C++20 runtime for governing coherence, consistency, ownership,
invalidation, synchronization, and authority across accelerator, host, CXL-class, and distributed
memory.**

Coherence Fabric 1.0.0 — Summon Software Labs.

---

## The systems boundary

Coherence Fabric is **not** a capacity pool, an allocator, a transfer engine, a tiering policy
engine, or a cache directory. Its boundary is semantic:

> When the same logical state may exist in multiple memory domains, which copy is authoritative,
> which copies are current, what consistency guarantees apply, what synchronization is required,
> and who is still authorized to change or observe it?

### The central principle: location is not coherence

Two memory regions referring to the same logical state are not coherent merely because both exist.

* A copy is not current because it was once synchronized.
* A replica is not authoritative because it contains the newest value someone has observed.
* A successful transfer is not itself publication.
* A completed write is not automatically visible everywhere.
* A cache hit is not proof of freshness.
* A process restart must not revive stale ownership.
* UNKNOWN remains UNKNOWN.

Coherence Fabric makes these distinctions explicit, inspectable, deterministic, generation-bound
and recoverable.

### What Coherence Fabric owns

* logical coherence objects and their generations;
* memory-region participation in coherence domains;
* authoritative ownership, read authority and write authority;
* shared/read-mostly and exclusive writable state;
* dirty-state tracking, publication and invalidation;
* synchronization plans and completion records;
* flush and revalidation requirements;
* version and generation progression;
* replica freshness and consistency policy;
* conflict detection and stale-reference rejection;
* fence semantics;
* recovery after participant or coordinator failure;
* evidence supporting every coherence claim;
* deterministic inspection and persistent coherence metadata.

### What Coherence Fabric explicitly does not own

Allocation and representation across heterogeneous memory domains (Unified Buffer); how bytes are
copied, staged, routed or pipelined (Transfer Fabric); discovery, pooling, capacity, locality and
lifecycle of expanded memory (CXL Fabric / Memory Expansion Fabric); discovery and location of
cached distributed state (Distributed Cache Directory); connectivity and cost description (Topology
Fabric); durable execution checkpoints (Checkpoint Fabric).

The runtime also does **not** implement and does not claim: CPU cache protocols, GPU hardware cache
protocols, CXL.cache, MESI/MOESI hardware machinery, PCIe coherency, NVLink hardware coherency, or
any other physical protocol.

Where an adjacent concern is needed for a proof, it appears as an explicitly named, narrow
substrate: the TCP control plane, the region store used to hold real bytes, and the CUDA adapter.
None of them change the boundary.

---

## Core coherence model

A **coherence object** is one logical state identity. It has a generation, an extent, a policy, a
monotonic authoritative version and a set of physical **replicas** (regions). An address is never an
object identity: the same address reused later never resurrects an earlier coherence object, because
every region carries its own `RegionGeneration` and binds to an explicit `ObjectGeneration`.

### Replica coherence states

| State | Meaning |
| --- | --- |
| `unknown` | No determination is possible. Fails closed under strict policy. |
| `invalid` | Contents must not be read; no valid data is present. |
| `stale` | Contents are known to predate the authoritative version. |
| `current` | Contents match the authoritative version **and** carry live evidence of that fact. |
| `sync_required` | A synchronization must complete before currentness can be established. |
| `revalidation_required` | Dynamic evidence was lost (restart, boot change, epoch change). |
| `dirty` | Holds an unpublished modification of the authority holder. |
| `fenced` | The owning participant was fenced; contents are not a source of truth. |
| `retired` | Terminal. The replica never returns to a live state. |

These are software-governed authority and visibility states, not hardware cache-line states.

### Lifecycles

* Object: `created -> active -> quiescing -> recovery_required -> retired`.
* Region: `registered -> active -> quiescing -> retired`.
* Participant: `observed -> admitted -> active -> degraded -> fenced -> retired`.
* Domain: `created -> active -> quiescing -> recovery_required -> retired`.

Every transition is validated. Impossible combinations are rejected, not repaired. A retired replica
can never become writable or current again; a fenced participant is never un-fenced (it is
re-admitted under a fresh `ParticipantBootId`, which is a distinct incarnation).

### Identity and generation model

`CoherenceDomainId`, `ObjectId`, `ObjectGeneration`, `RegionId`, `RegionGeneration`,
`ReplicaId`, `ReplicaGeneration`, `ParticipantId`, `ParticipantBootId`, `CoordinatorEpoch`,
`OwnershipGeneration`, `VersionId`, `PublicationId`, `InvalidationId`, `SyncOperationId`,
`LeaseId`, `PolicyId`, `PolicyGeneration`, `EvidenceId`, `EvidenceGeneration`, `DecisionId`,
`RequestId`.

Each is a distinct C++ type. A value from one authority domain cannot be silently substituted for
another. `ParticipantBootId`, `SessionId` and content digests are 128-bit identities drawn from
the operating system cryptographic random source; they are never derived from a process id, thread
id, address or timestamp, and authority is never restored because an integer or an address was
reused.

---

## Consistency modes

Only modes with precise, enforced semantics are exposed. Requesting anything else fails closed with
`unsupported`.

**`strict` (default).** A reader requires live evidence that the replica matches the authoritative
version, and the object must have an authoritative version at all. A publication must commit before
any reader can observe the new version. Missing or ambiguous evidence yields `read_blocked` or
`unknown`; the runtime never guesses.

**`release_acquire`.** A writer publishes with release ordering: the publication becomes visible
only after every mandatory invalidation has been acknowledged, and a reader must acquire at or
beyond the published version before a replica may serve a current read.

**`snapshot`.** Readers bind to an explicit snapshot version. A replica whose recorded version is
at or beyond the snapshot version may serve the read, and the decision reports **the snapshot
version** as the authoritative version, not the newest one. A snapshot cannot bind to a version that
was never published.

**`eventual`.** Readers may serve stale contents, bounded by policy. The decision always reports
the region version (latest known) **and** the authoritative version separately, so "latest known" is
never presented as "authoritative current". Exceeding the configured bound escalates to
`read_blocked`.

Every mode defines: when writes become publishable, when reads become valid, what synchronization
establishes visibility, whether concurrent writers are allowed, conflict behaviour, ordering
guarantees, restart behaviour, and what UNKNOWN means.

---

## Authority and write semantics

Write authority binds to `CoordinatorEpoch`, `ObjectGeneration`, `RegionGeneration`,
`ParticipantBootId`, `OwnershipGeneration` and `PolicyGeneration`. Any mismatch is rejected
with a distinct status code.

The implemented write lifecycle is:

1. resolve object, participant, generations;
2. reject if a publication is pending durability or an unpublished modification exists;
3. resolve conflicting readers and replicas;
4. create generation-bound invalidations for every incompatible live replica and mark the transfer
   `transfer_pending`;
5. wait for acknowledgements (a mutation attempt before acknowledgement is refused);
6. `exclusive_writer` becomes effective;
7. mutate real bytes;
8. `mark_dirty` records the unpublished modification and its base version;
9. `publish` reserves a version, makes the publication record durable, then commits;
10. readable/current replicas are updated as policy allows;
11. `release` advances the ownership generation so the previous authority cannot be reused.

At most one participant incarnation holds exclusive write authority for an object generation.
Multi-writer semantics are **not** implemented: a policy that declares them is rejected at
validation time with `unsupported`, so multi-writer operation cannot happen by accident.

Write completion is never publication. The runtime distinguishes: write completed locally, new
version published, new version synchronized elsewhere, new version authoritative.

---

## Read semantics

A read decision is never a boolean. It answers: which replica was selected, what state it is in,
what version it holds, what the authoritative version is, how stale it is, whether synchronization
is required, whether stale reads are permitted, which evidence supports the claim, and which
generations produced the decision.

Outcomes: `read_current`, `read_after_sync`, `read_stale_allowed`, `read_blocked`,
`unknown`, `unsupported`.

* A replica that has never been observed cannot satisfy a current read.
* An invalidated, fenced or retired replica cannot satisfy a current read.
* An object with no authoritative version cannot certify any replica as current.
* A missing freshness proof never becomes `read_current`.

---

## Publication, invalidation and synchronization

**Publication** is transactional. The engine reserves a `PublicationId` and the next `VersionId`,
writes the pending publication record to the durable journal, and only then commits visibility. If
the journal write fails, the publication aborts and no version becomes authoritative. If the
coordinator dies between the durable record and the visible commit, recovery completes **the same**
publication and version rather than inventing a second one. Publication is idempotent per
`RequestId`, and that record is itself durable, so a retried request is answered identically across
a restart.

**Durability point (exact):** the successful, flushed append of the pending publication record to the
journal (or, with group commit, the successful flush covering the group). The in-memory commit of
that same `PublicationId` follows immediately. With `publication_durability = ephemeral` — or
with no durable store attached at all — a publication is reported as non-durable and the CLI says so.

**Invalidation** is explicit and generation-bound. An invalidation binds to object generation,
target region generation, replica generation, published version, ownership generation and
coordinator epoch. Invalidating generation N can never touch a replacement at N+1; a late
acknowledgement for a replaced replica is recorded as `superseded`, never applied. Invalidating an
inert (retired or fenced) replica is satisfied vacuously and recorded as `superseded`.

**Synchronization** is represented explicitly: source and destination regions with their
generations, source version, destination prior version, the byte extent to move, the expected
content fingerprint, the authority and policy generations, the transport dependency and the
expected postcondition. A plan is not a completion. Completion is verified:

* the destination participant incarnation must be the one the plan was issued to;
* the reported version must be the planned version;
* the authoritative version must not have advanced underneath the transfer;
* the observed content fingerprint must equal the published fingerprint.

A completion that moves the wrong extent, or whose bytes do not match, is recorded as failed and the
replica is **not** made current. A transfer that finishes against a superseded version marks the
replica stale instead of current. Duplicate completions are idempotent. When a participant
disappears mid-transfer the outcome is recorded as `outcome_unknown`, never as success.

---

## Dirty state and process death

Dirty state is first-class: which region owns it, the version it diverged from, whether it was
published, whether mandatory copies are stale, and whether recovery can determine the outcome.

* A writer that dies before publication can never cause the system to invent a successful
  publication. The loss is reported as `dirty_unknown` or `dirty_lost` according to policy, and
  `dirty_loss_policy = require_recovery` (the strict default) moves the object to
  `recovery_required`, where it stays until an operator resolves it explicitly with
  `object resolve-recovery`. Resolution records that the unpublished modification was lost and
  leaves the last authoritative version standing.
* A participant that dies after the publication record became durable but before it received a
  response cannot cause a second independent version on blind retry: the request identity is
  durable, so the retry is answered with the original publication and version.
* A fenced or killed participant's authority is revoked immediately; its replicas become `fenced`
  and its outstanding invalidations are satisfied vacuously.

---

## UNKNOWN

UNKNOWN is first-class and fails closed. It arises from missing freshness evidence, a participant
that disconnected during synchronization, an ambiguous write outcome, a backend that cannot answer,
persisted dynamic state after a restart, uncertain transfer completion, and unavailable physical
capability. It is never silently mapped to current, clean, success, visible, synchronized or
healthy.

---

## Evidence model

Every currentness claim has provenance: runtime observation, verified byte comparison, backend
completion, process lifecycle event, explicit acknowledgement, persisted metadata, hardware query or
synthetic fixture, each classified `real`, `synthetic` or `unsupported`.

Two rules are enforced, not merely documented:

1. Persisting evidence does not make dynamic evidence perpetually current. Restored
   `persisted_metadata` evidence yields `revalidation_required` and can never establish
   currentness on its own.
2. Process-local evidence does not survive a participant restart. A new `ParticipantBootId`
   invalidates every observation gathered under the previous boot.

Synthetic evidence cannot support a claim over a real memory domain: a replica whose backend can
only produce fixtures is reported as `unsupported` for currentness purposes.

---

## Policy

Policy is a versioned, first-class object — never process-global configuration. Every
authority-bearing decision records the `PolicyId` and `PolicyGeneration` under which it was
taken, so a policy change (which advances `PolicyGeneration`) makes earlier derived decisions
stale.

Fields: consistency model, write ownership mode, publication durability, conflict behaviour,
recovery policy, stale-read policy and bound, dirty-loss policy, evidence maximum age (in logical
operations, never wall-clock time), whether publication requires invalidation acknowledgements,
whether synchronization completion is required for currentness, the permitted memory domains, and
whether a content fingerprint is mandatory.

---

## Persistence and recovery

A durable directory holds two files:

* `coherence-fabric.snapshot` — the full durable image, written to a temporary sibling, flushed,
  then atomically replaced;
* `coherence-fabric.journal` — length-framed append-only records with magic, format version, entry
  kind, log position, declared length and CRC-32C.

The snapshot additionally carries a header CRC and a SHA-256 trailer over header plus payload.
Readers validate magic, format version, schema, declared lengths (against both the configured bound
and the bytes actually present) and integrity before anything is allocated, and reject trailing
garbage.

Persisted: domain identity, object identities and generations, policy, region identities and
generations, ownership/publication generations, durable version metadata, participant durable
identity, outstanding invalidations, in-flight synchronizations, pending publications and committed
publication request identities. Dynamic authority — read grants, writer assignments, live sessions —
is deliberately **not** persisted.

On coordinator restart:

* the authority epoch always advances (from N to at least N+1);
* every session, boot incarnation and authority grant from before the restart is invalidated;
* every replica is downgraded to `revalidation_required` and receives `persisted_metadata`
  evidence, which the evidence model refuses to treat as proof;
* in-flight synchronizations are classified `outcome_unknown`;
* outstanding invalidations are discharged by the conservative downgrade and recorded as such;
* a durable pending publication is completed under its original publication and version identity
  when the policy authorises it, and otherwise moved to `recovery_required` for an operator;
* derived state (participant region accounting) is recomputed rather than trusted;
* a durable record whose map key disagrees with the identity embedded in its body is ignored and
  reported;
* an invariant audit runs.

---

## Distributed architecture

Three executables:

* `coherence_coordinator` — owns distributed coherence authority;
* `coherence_agent` — a participant process that holds real memory regions;
* `coherence_cli` — deterministic inspection and proof driver (embedded engine or remote session).

Framed TCP control plane on loopback, 60-byte fixed header: magic, protocol version, message type,
flags, session identity (128-bit), the coordinator epoch the sender believes is current, a
per-session monotonic sequence, a correlation identity, the body length and the body CRC-32C.

Message families: `hello`/`hello_ack`, `create_domain`, `register_participant`,
`register_object`, `register_region`, `acquire_read`, `acquire_write`, `mark_dirty`,
`publish`, `invalidate`, `acknowledge_invalidation`, `sync_begin`, `sync_complete`,
`sync_fail`, `release`, `revalidate`, `resolve_recovery`, `query`, `snapshot`, `audit`,
`fence_participant`, `retire_region`, `retire_object`, `set_policy`, `list_*`,
`decision_log`, `recovery_info`, `shutdown`, `ping`.

Defences: malformed frames, truncation, oversized declared lengths (rejected before allocation),
invalid enum values, undeclared message types, body CRC mismatch, protocol/schema version mismatch,
stale epochs, stale boots, stale generations, duplicate identities, replayed frames (non-advancing
session sequence), duplicate correlation identities, reconnects (connection replacement closes the
previous session first), unknown messages, disconnect during a mutation, coordinator restart and
half-open teardown.

An ordinary TCP socket is not RDMA, not a remote direct memory path and not a coherent
interconnect. Coherence Fabric never describes it as one.

---

## Real host-memory proof

`cf_test_memory` and `coherence_cli demo` operate on actual bytes:

* pageable host buffers allocated in-process, mutated and compared;
* a second host replica brought to current only by a copy that is verified by content comparison;
* a Windows named file mapping observed through two independent handles, proving that a real
  cross-process byte path exists;
* pinned host memory exercised when the platform supports it (reported as UNSUPPORTED otherwise);
* the runtime **refusing** to certify currentness when the destination bytes do not match the
  published fingerprint, when the source and destination alias the same pages (nothing moved), or
  when a synchronization completes against a superseded version.

Metadata transitions are always tied to content transitions: the fingerprint recorded by a
publication is the fingerprint of the bytes that were published.

---

## Real CUDA proof

`cf_test_cuda` and the `cuda_host_device_coherence` example use the actual device present in the
environment (an NVIDIA GeForce RTX 5090, compute capability 12.0, reported by the adapter's own
probe):

device discovery via the real runtime; `cudaMalloc`; host source data; real H2D; a real kernel
mutation launched on the device; stream synchronization; real D2H; CPU/reference parity against a
deterministic host implementation of the same transform; coherence metadata transitioning with the
real data; invalidation of the host-side copy after the device gained write authority; D2H
resynchronization; publication of the device's bytes; cleanup; and a device memory baseline check.

This is **software-governed** coherence over real CUDA memory and transfers. No GPU hardware cache
protocol is implemented or claimed, and a device buffer is never treated as coherent with host
memory merely because both allocations exist in one process.

---

## CXL-class and remote memory

A CXL-class memory domain exists in the model with its own identity, locality metadata, generation
changes, ownership transitions, synchronization policy and failure/revalidation behaviour. It is
exercised through a **synthetic** backend that allocates ordinary host bytes and labels the domain
`cxl_class` and the evidence `synthetic`.

Remote/distributed memory is modelled at the software level: a `remote` memory domain, a real
participant process on the other end of the control plane, explicit synchronization plans and
verified completions. The cross-process byte path used by the multiprocess proof is a Windows named
file mapping on one host. No RDMA path and no second physical node exist, so RDMA coherence and
physical multi-node memory are UNSUPPORTED and are not claimed.

---

## REAL / SYNTHETIC / UNSUPPORTED

| Capability | Status | Evidence |
| --- | --- | --- |
| Windows process behaviour | REAL | coordinator and two agent OS processes launched, killed and restarted by tests |
| TCP loopback control plane | REAL | framed protocol validation tests and the multiprocess proof |
| Host pageable memory | REAL | byte-level allocation, mutation, comparison |
| Host shared memory (Windows named file mapping) | REAL | two independent handles observing the same pages |
| Host pinned memory | REAL when the platform grants it, otherwise UNSUPPORTED | `VirtualLock` probe in `cf_test_memory` |
| In-process byte copy with content verification | REAL | fingerprint comparison after every copy |
| Persistence and integrity | REAL | file store, CRC-32C, SHA-256, corruption and truncation tests, real process exit and reload |
| Coordinator kill and restart | REAL | epoch advance, pre-restart authority rejection, conservative recovery |
| Participant kill, fencing and reincarnation | REAL | stale boot refused, fresh boot accepted, old boot cannot mutate |
| RTX 5090 CUDA memory and transfers | REAL | device probe, `cudaMalloc`, H2D, kernel mutation, D2H, CPU parity, baseline return |
| CXL-class memory domain | SYNTHETIC | host bytes labelled `cxl_class` with `synthetic` evidence |
| Remote memory participant | participant processes REAL; byte path SYNTHETIC on one host | shared-mapping staging plus framed TCP control |
| Physical CXL hardware semantics | UNSUPPORTED | no CXL device present |
| CXL.cache / CXL.mem device behaviour | UNSUPPORTED | not implemented |
| Fabric-attached pooled memory, CXL switches | UNSUPPORTED | no hardware |
| RDMA-coherent memory | UNSUPPORTED | no RDMA path; TCP is not RDMA |
| Physical multi-node coherent memory | UNSUPPORTED | single host |
| CPU hardware cache-protocol control | UNSUPPORTED | not implemented |
| GPU hardware cache coherence | UNSUPPORTED | software-governed coherence only |
| NVLink hardware coherence | UNSUPPORTED | not present |

---

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and (optionally) a CUDA toolkit for the CUDA
adapter and proof. The core runtime never requires CUDA, CXL hardware, RDMA or any other optional
SDK.

`~
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
`~

Options: `COHERENCE_FABRIC_BUILD_TESTS`, `COHERENCE_FABRIC_BUILD_EXAMPLES`,
`COHERENCE_FABRIC_BUILD_BENCHMARKS`, `COHERENCE_FABRIC_BUILD_APPS`,
`COHERENCE_FABRIC_ENABLE_CUDA`, `COHERENCE_FABRIC_ENABLE_ASAN`,
`COHERENCE_FABRIC_WARNINGS_AS_ERRORS` (on by default), `BUILD_SHARED_LIBS`.

The build is clean under MSVC `/W4 /WX /permissive-`; there are zero first-party warnings.

## Test

`~
ctest --test-dir build --output-on-failure
`~

Twelve suites: `cf_test_core`, `cf_test_semantics`, `cf_test_protocol`,
`cf_test_persistence`, `cf_test_property`, `cf_test_concurrency`, `cf_test_adversarial`,
`cf_test_memory`, `cf_test_invariants`, `cf_test_distributed`, `cf_test_scale`, and
`cf_test_cuda` when the CUDA adapter is built.

Every case has a stable identifier, runs exactly by name with `--case NAME`, lists with `--list`,
and emits immediately flushed `BEGIN`, `PHASE`, `PASS`/`FAIL` markers so that a hang is
localised to a named case and a named phase rather than hidden behind a timeout. No case relies on a
wall-clock deadline or a sleep for correctness.

## Install and consume

`~
cmake --install build --prefix /some/prefix
`~

The installed package exports `CoherenceFabric::coherence_fabric` (and
`CoherenceFabric::coherence_fabric_cuda` when built) with a version file and a correct include
layout, so a downstream project only needs:

`~cmake
find_package(CoherenceFabric CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE CoherenceFabric::coherence_fabric)
`~

## Examples

Nine runnable examples, all executed by the release validation:

`exclusive_write_shared_reads`, `host_replica_resync`, `publication_versions`,
`shared_memory_participant`, `stale_authority_rejection`, `recovery_revalidation`,
`synthetic_cxl_domain`, `strict_versus_relaxed_reads`, `cuda_host_device_coherence`.

They exercise real semantics: they allocate real memory, copy real bytes, verify fingerprints, and
assert outcomes.

## CLI

`~
coherence_cli [--state DIR | --coordinator HOST:PORT] COMMAND [options]
`~

Commands: `status`, `domain create|show`, `object create|show|list|retire|resolve-recovery`,
`policy show`, `region register|list|retire`, `acquire read|write`, `mark-dirty`,
`publish`, `invalidate`, `ack-invalidation`, `sync-begin`, `sync-complete`, `release`,
`revalidate`, `participant register|list|show|fence`, `snapshot`, `decisions`, `recovery`,
`audit`, `verify`, `demo`.

Output is deterministic. `coherence_cli demo` runs a complete lifecycle against real bytes and
exits non-zero if any expected outcome does not occur, so it is a proof rather than a transcript.
`coherence_cli audit` reports zero violations on a healthy runtime.

## Benchmarks

`~
coherence_benchmarks [--scale N] [--no-large]
`~

Completed-operation measurements at scales 10, 100, 1,000, 10,000 and a 100,000-replica run:
replica registration, read-authority lookup, currentness query, state transition, serialization
encode/decode, snapshot creation, invariant audit, durable save/load, writer handoff, synchronization
plan generation, durable publication, and contention under eight concurrent readers. Every
measurement surrounds the whole operation; no submit-only path is reported as completed throughput.

---

## Architecture

`~
include/coherence/          public headers
  ids, status, enums, bytes, export, version
  evidence, policy, model, decision
  serialize, codec, persistence, platform_file
  engine, transport, protocol, client, coordinator, agent
  adapters/region_store, adapters/cuda_memory
src/                        implementation
apps/                       coordinator, agent, cli
tests/                      twelve suites plus the ASan instrumentation probe
examples/                   nine runnable examples
benchmarks/                 completed-work benchmarks
docs/                       architecture, semantics, protocol, persistence, capability matrix
`~

### Concurrency contract

One non-recursive mutex guards engine state. There are no read locks to upgrade, no nested
acquisition and no re-entrancy: no callback, virtual dispatch into user code, logging sink or
blocking I/O is performed while the mutex is held *except* for the durability writes described
below, which are performed by the store under its own separate mutex.

* Metadata transitions (registration, policy change, invalidation, synchronization records) build
  their journal entries, write them as a **group** with a single durability barrier, and only then
  apply them to durable and live state. A reader can therefore never observe metadata that the
  durable record does not contain.
* Publication uses two phases. The pending publication is reserved under the state lock; the
  durability barrier is issued with the state lock **released**; the reservation is re-validated and
  visibility is committed under the state lock. A reader never observes a version before its durable
  record exists, and a compaction is never allowed to run while a reservation is in flight, so a
  compaction can neither make an uncommitted publication durable nor drop a durable one.
* Lock order is always engine state, then store. The store mutex is never held while acquiring the
  engine lock.

### Determinism

No decision depends on unordered-map iteration order, pointer order, thread timing, filesystem
order, process ids, wall-clock ordering, random-device output or backend enumeration order. Identity
allocation is monotonic per coordinator; rendering, hashing and serialization are canonical;
randomized tests use a recorded seed.

### Error semantics

Stable machine-readable status codes, never generic booleans: `invalid_argument`,
`invalid_state`, `invalid_transition`, `unsupported`, `capacity_exceeded`,
`duplicate_identity`, `unknown_*`, `stale_epoch`, `stale_boot`,
`stale_object_generation`, `stale_region_generation`, `stale_replica_generation`,
`stale_ownership`, `stale_policy`, `stale_publication`, `stale_invalidation`,
`stale_sync_operation`, `stale_session`, `replayed_request`, `not_authoritative`,
`read_not_current`, `write_conflict`, `exclusive_writer_conflict`, `not_write_authorized`,
`fenced`, `retired`, `sync_required`, `revalidation_required`,
`invalidation_outstanding`, `evidence_missing`, `evidence_stale`, `dirty_unpublished`,
`dirty_lost`, `recovery_required`, `quiescing`, `shutting_down`, `outcome_unknown`,
`content_mismatch`, `integrity_failure`, `corruption_detected`, `truncated_input`,
`trailing_garbage`, `oversized_input`, `protocol_violation`, `unsupported_schema`,
`persistence_failure`, `transport_failure`, `connection_closed`, `timeout`,
`internal_invariant_violation`, `internal_error`.

### Invariant auditor

Callable from tests and from `coherence_cli audit`. It checks identity uniqueness and index
agreement, generation monotonicity, legal lifecycle and coherence-state combinations, at most one
exclusive writer per object generation and no coexistence with a live read grant held by another
incarnation, current-version agreement, stale-region exclusion, ownership validity, participant boot
validity, coordinator epoch validity, policy-generation consistency, no retired holder retaining
live authority, no missing referenced object or region, no decision derived from stale generations,
no publication referencing non-existent authority, persistence health, and accounting closure.

---

## Known limitations

* Coherence is governed by software. No hardware cache protocol is implemented, controlled or
  claimed anywhere.
* Physical CXL hardware, CXL.cache, CXL.mem, fabric-attached pooling and CXL switches are absent;
  the CXL-class domain is synthetic.
* RDMA and physical multi-node coherent memory are absent. The distributed proof uses loopback TCP
  and a single host.
* Multi-writer semantics are not implemented and a policy that declares them is rejected.
* Synchronization moves bytes internally only for the simple supported paths (in-process copies and
  a shared-mapping staging path). A general data plane is deliberately out of scope: the runtime
  exposes plans, authority and completion records, and delegates actual transfer to an adjacent
  runtime.
* Leases are logical read grants with no wall-clock expiry. Evidence age is measured in coordinator
  logical operations, never in time.
* `ephemeral` publication durability intentionally has no durable record; a restart loses the
  version and the CLI reports the mode.
* The scale proof covers 10,000 objects and replicas with a full save/load/audit cycle. Larger
  deployments are architecturally supported but have not been measured here.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
