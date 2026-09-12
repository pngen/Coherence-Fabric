# Coherence Fabric control-plane protocol

## Transport

Ordinary stream TCP over loopback. This is not RDMA, not a remote direct memory path and not a
coherent interconnect, and the runtime never describes it as one.

## Frame

60-byte fixed header, little-endian, then the body:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic, `CFG1` |
| 4 | 2 | protocol version |
| 6 | 2 | message type |
| 8 | 4 | flags |
| 12 | 8 | session identity, high half |
| 20 | 8 | session identity, low half |
| 28 | 8 | coordinator epoch the sender believes is current |
| 36 | 8 | per-session monotonic sequence |
| 44 | 8 | correlation identity |
| 52 | 4 | body length |
| 56 | 4 | body CRC-32C |

The body length is validated against the configured bound and the bytes actually available before
anything is allocated. The CRC is verified before the body is decoded. Every record inside the body
carries a version, and every enumeration value is validated against its declared members: an
undeclared value is rejected, never defaulted.

## Handshake

1. The client sends `hello` with protocol version, schema, build string, participant name,
   128-bit boot identity, node label, a nonce and an observer flag.
2. The coordinator validates protocol and schema version, refuses if it is shutting down, replaces
   any existing session for that participant (shutting the previous socket down first), fences a
   superseded boot incarnation, registers the participant, and replies with `hello_ack` carrying
   the session identity, its current epoch and the participant identity.
3. Every later request carries that session identity and that epoch. A different epoch is refused
   with `stale_epoch`.

## Messages

`hello`, `hello_ack`, `response`, `goodbye`, `create_domain`, 
`register_participant`, `register_object`, `register_region`, `acquire_read`, 
`acquire_write`, `mark_dirty`, `publish`, `invalidate`, 
`acknowledge_invalidation`, `sync_begin`, `sync_complete`, `sync_fail`, 
`release`, `revalidate`, `resolve_recovery`, `query`, `snapshot`, 
`audit`, `fence_participant`, `retire_region`, `retire_object`, 
`set_policy`, `list_participants`, `list_objects`, `list_regions`, 
`list_invalidations`, `decision_log`, `recovery_info`, `shutdown`, `ping`.

## Response convention

Every response body begins with the envelope: a status code, a message and a detail string. The
control client decodes and validates the envelope while receiving and removes it from the payload,
so a payload holds only the operation-specific part: a canonical binary record for operations that
return structured data, or one length-prefixed text block for inspection operations.

## Defences

| Attack | Defence |
| --- | --- |
| malformed or truncated frame | magic, version, type and length validation; truncation is refused |
| oversized declared body | rejected against the configured bound before allocation |
| corrupt body | CRC-32C mismatch is refused before decoding |
| undeclared enum or message type | validated against the declared members and refused |
| replayed frame | the per-session sequence must strictly increase |
| duplicate correlation identity | retained correlation window rejects a repeat |
| stale coordinator epoch | refused with `stale_epoch` |
| stale participant boot | refused with `stale_boot` |
| stale object, region or replica generation | refused with the matching stale code |
| duplicate identity | refused with `duplicate_identity` |
| connection replacement | the previous session is shut down before the new one is admitted |
| disconnect during a mutation | the participant is fenced; authority is revoked, replicas are fenced, outstanding invalidations are superseded, in-flight transfers become outcome unknown |
| coordinator restart | the epoch advances and every pre-restart request is refused |
| half-open teardown | shutdown and close are both issued, and connection threads are joined |
