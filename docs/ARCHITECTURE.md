# Coherence Fabric architecture

## Boundary and layering

```
  applications            coherence_coordinator | coherence_agent | coherence_cli
  control plane           protocol (framing) | transport (TCP) | client | coordinator | agent
  runtime                 engine | evidence | policy | model | decision | invariants
  durable state           persistence (journal + snapshot) | codec | serialize | platform_file
  adapters                region_store (host, shared, synthetic CXL) | cuda_memory
  foundations             ids | status | enums | bytes (CRC-32C, SHA-256, PRNG)
```

The runtime layer never depends on the control plane; the control plane never decides coherence
outcomes, it only carries requests to the engine and responses back.

## Engine structure

`CoherenceEngine` owns a private `Impl` that holds:

* live records: domains, participants, objects, regions, invalidations, synchronizations, policies;
* deterministic indexes: domain name, participant name, object name per domain, region name per object;
* the durable image, which mirrors exactly what the journal contains;
* in-flight publication reservations, deliberately excluded from durable state until their record has actually been appended;
* identity counters, all monotonic per coordinator;
* a bounded decision log.

Reads and writes of that state are serialised by one non-recursive mutex. No callback, virtual
dispatch, logging sink or blocking I/O runs under it, with the documented exception of the grouped
durability writes that the store performs under its own separate mutex.

## Ordering rules that carry the semantics

**Registration and metadata changes.** Under the state lock: validate, build the journal entries,
write the whole group with one durability barrier, then apply the entries to the durable image and
to live state. A reader therefore never sees metadata that the durable record does not contain, and
a failed append leaves both durable and live state untouched.

**Publication.** Three phases:

1. under the state lock: validate authority, generations, base version, outstanding invalidations
   and the content fingerprint; reserve a `PublicationId` and the next `VersionId`;
   record the reservation as in-flight; release the lock;
2. with the lock released: append the pending publication record and wait for the durability barrier;
3. under the state lock: re-validate that the reservation is still the same object, ownership
   generation and base version; then commit visibility, mark other replicas stale, and append the
   commit markers.

If step 2 fails, the publication aborts and nothing becomes authoritative. If step 3 finds the
world has moved, the reservation is discarded and the durable pending record is cleared, so
recovery cannot resurrect it. If the commit markers in step 3 cannot be appended, the version is
still authoritative because the durable pending record fixes the exact publication and version, and
recovery converges on the same outcome.

**Compaction** never runs while a publication reservation is in flight, so a compaction can neither
make an uncommitted publication durable nor drop a durable one.

**Lock order** is always engine state, then store. The store mutex is never held while acquiring
the engine lock.

## Determinism

Identity allocation is monotonic. Records are held in ordered maps. Rendering, hashing and
serialization are canonical. Randomized tests record their seed. No decision is derived from
unordered iteration, pointer order, thread timing, filesystem order, process ids, wall-clock time,
random-device output or backend enumeration order.

## Selection rules

* Read replica selection: the lowest region identity that is current, else the lowest region
  identity that holds any contents. A re-validated cache of the last selection keeps the common
  path constant time; if the cache is stale the selection falls back to a full scan, so a stale
  cache costs time and never changes the answer.
* Synchronization source selection: the lowest region identity that is current at the authoritative
  version, excluding the destination.
* Invalidation targets: every live replica of the object other than the writer's own.

## Failure containment

* A fence always takes effect in memory even if the durable record cannot be written: failing to
  revoke authority because a disk write failed would be strictly less safe than failing to record
  the revocation, so the error is reported and the store is marked unhealthy.
* Recovery ignores a durable record whose map key disagrees with the identity embedded in its body,
  and reports the discrepancy.
* Recovery recomputes derived state (participant region accounting) rather than trusting it.

## Resource ownership

Sockets, files, mappings, device allocations, threads and child processes are all owned by RAII
types that release on destruction. Connection threads are joined before the coordinator returns
from `stop()`; no detached thread is ever created; a blocking accept is interrupted by
shutting the listener down; a blocking receive is interrupted by shutting the socket down.
