# Coherence Fabric persistence

## Files

| File | Content |
| --- | --- |
| `coherence-fabric.snapshot` | the full durable image, atomically replaced |
| `coherence-fabric.journal` | length-framed append-only records |

## Snapshot format

```
  u32 magic "CFS1"
  u16 format version
  u16 schema
  u64 payload length
  u32 payload CRC-32C
  u32 header CRC-32C (over the preceding bytes with this field zeroed)
  u64 sequence
  payload
  sha256(header || payload)      -- 32 bytes
```

A reader validates the magic, the format version, the schema, the declared payload length against
the configured bound, the header CRC, the payload CRC and the SHA-256 trailer, and requires that the
file size equals the declared size exactly. Trailing bytes are rejected rather than ignored.

## Journal record format

```
  u32 magic "CFJ1"
  u16 format version
  u16 entry kind
  u64 log position
  u32 payload length
  u32 payload CRC-32C
  payload
```

Replay reads records in order. The first record that fails framing, integrity or decoding stops
replay: the journal is a strictly ordered log, so nothing after a damaged record is trusted. That is
reported as a truncated tail with the count of records actually applied.

## Durability point

A publication becomes authoritative only after its pending publication record has been appended and
the durability barrier has returned success. Metadata transitions are written as a group with a
single barrier. The store can be configured with `barrier_per_append = false` for bulk loading,
in which case every record is still written before the state it describes becomes observable and the
caller takes responsibility for an explicit flush; the scale proof uses that mode and flushes once.

## Atomic replacement

A snapshot is written to a temporary sibling file, flushed, then moved over the target with
replace and write-through semantics. The journal handle is closed before the journal is rotated, so
no handle outlives the replacement. On failure the temporary file is removed and the previous
snapshot remains in place.

## What is persisted

Domain identity, object identities and generations, policies, region identities and generations,
ownership and publication generations, durable version metadata, participant durable identity,
outstanding invalidations, in-flight synchronizations, pending publications, and committed
publication request identities.

## What is deliberately not persisted

Read grants, writer assignments and live sessions. They are dynamic authority; restoring them would
assert authority that nothing can support after a restart.

## Recovery

1. Load and validate the snapshot, then replay the journal.
2. Advance the coordinator epoch to at least previous + 1.
3. Restore domains, participants, policies, objects, regions, invalidations and synchronizations.
   A record whose map key disagrees with the identity embedded in its body is ignored and counted.
4. Invalidate every session: participants become degraded, their sessions and replay counters are
   cleared.
5. Revoke all dynamic object authority and set it to revalidation required.
6. Classify pending publications. With `complete_durable_pending` a durable pending record is
   completed under its original publication and version identity, and its request identity is
   recorded so a retry is answered identically. With `conservative` the object is moved to
   recovery required and an operator must resolve it. A pending publication with no durable record is
   aborted rather than assumed to have committed.
7. Classify unpublished modifications as dirty unknown or dirty lost according to policy, and move
   the object to recovery required when the policy demands an explicit decision.
8. Downgrade every replica to revalidation required and attach persisted-metadata evidence, which the
   evidence model refuses to treat as proof of currentness.
9. Classify in-flight synchronizations as outcome unknown.
10. Discharge outstanding invalidations through the conservative downgrade and record them as such.
11. Recompute derived state and write the recovered state back durably.
12. Compact and run an invariant audit.

## Recovery guarantees

* Dynamic currentness never survives a restart.
* Pre-restart authority is refused with `stale_epoch`.
* Pre-restart boot identities are fenced.
* A publication that was durable is never lost and is never duplicated.
* An unpublished modification is never turned into a successful publication.
