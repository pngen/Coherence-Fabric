# Coherence Fabric semantics

This document states the exact meaning of every state, transition and outcome. It is the contract
the implementation is tested against.

## Replica coherence states

| State | Exact meaning | May be read? | May be written? |
| --- | --- | --- | --- |
| `unknown` | No determination is possible. | No | No |
| `invalid` | No valid contents; the data must not be used. | No | No |
| `stale` | Contents are known to predate the authoritative version. | Only where stale reads are permitted | No |
| `current` | Contents match the authoritative version and carry live evidence. | Yes | Yes, by the authority holder |
| `sync_required` | A synchronization must complete first. | No | No |
| `revalidation_required` | Dynamic evidence was lost; a new observation is required. | No | No |
| `dirty` | Holds an unpublished modification of the authority holder. | By the holder, as latest known | Yes, by the holder |
| `fenced` | The owning participant was fenced. | No | No |
| `retired` | Terminal. | No | No |

Carrying live evidence means the evidence record passed `assess_evidence` against the
current coordinator epoch, the producing participant's live boot, the configured age bound and the
evidence class rules.

## Legal transitions

A transition is legal only as declared by `is_legal_coherence_transition`. Highlights:

* `retired` is terminal: nothing returns from it.
* `fenced` may only become `retired`, `invalid` or `revalidation_required`; it can never become `current`.
* `dirty` becomes `current` only through a publication, and `stale` when a publication is abandoned or superseded.
* `current` may become `stale` (a newer version published), `invalid` (invalidated), `dirty` (the authority holder began mutating), `fenced` or `retired`; it may also become `revalidation_required` when dynamic evidence is lost.
* `unknown` may be resolved into any determined state, which is how an unobserved replica becomes usable.

## Read outcomes

| Outcome | Meaning |
| --- | --- |
| `read_current` | The selected replica matches the authoritative version with live evidence, or (snapshot consistency) meets the requested snapshot version. |
| `read_after_sync` | The replica cannot serve a current read, but a current source replica exists, so an explicit synchronization can establish currentness. No read is authorized. |
| `read_stale_allowed` | Policy permits stale contents. The decision reports the region version (latest known) and the authoritative version separately. |
| `read_blocked` | The read must not proceed. |
| `unsupported` | The backend cannot produce evidence that could support the claim. |
| `unknown` | No determination is possible. |

A read decision always reports: the selected replica and its generation, its state and version, the
authoritative version, the staleness when it is known, whether synchronization is required, whether
stale reads are permitted, whether the evidence is fresh and of which class, the generations
involved, and a rationale.

## Write authority

Authority binds to coordinator epoch, object generation, region generation, participant boot
identity, ownership generation and policy generation. The engine grants authority only when all of
them match, and each mismatch has its own status code.

While an exclusive writer holds authority, no read lease is issued to any incarnation, because the
next write transfer invalidates every live replica regardless and a lease would assert that read
authority coexists with exclusive write authority.

Releasing write authority advances the ownership generation, so a lingering actor holding the
previous generation can never write again.

## Publication durability

| Durability | Authoritative after |
| --- | --- |
| `durable_metadata` | The pending publication record has been appended and the durability barrier has returned success; the in-memory commit of the same `PublicationId` follows immediately. |
| `ephemeral` | The in-memory commit only. There is no durable record; a restart loses the version. Reported as non-durable. |

## Synchronization completion rules

A completion is refused, and the replica is not made current, when:

* the caller is not the destination participant incarnation the plan was issued to;
* the reported version is not the planned source version;
* the destination region or its generation differs from the plan;
* the authoritative version advanced while the transfer was in flight (the replica becomes stale);
* the destination replica was retired (the transfer is cancelled);
* the content fingerprint of the destination does not equal the published fingerprint (the replica
  becomes `revalidation_required`).

A duplicate completion is reported as an idempotent replay and applies no second effect.

## Evidence ageing

Evidence is invalidated by events: a coordinator epoch advance, a participant boot change, an
ownership change, a publication that supersedes the replica, an invalidation, or retirement. A
policy may additionally set `evidence_max_age_operations` to expire evidence after a number
of coordinator logical operations. Zero, the default, means no age bound: evidence never expires
merely because operations happened. The mechanism is tested both ways.

## UNKNOWN

UNKNOWN is produced by missing evidence, an in-flight transfer whose outcome cannot be established,
an ambiguous write outcome, a backend that cannot answer, restored dynamic state, and unavailable
hardware. Under strict policy it fails closed. It is never reported as current, clean, successful,
visible, synchronized or healthy.
