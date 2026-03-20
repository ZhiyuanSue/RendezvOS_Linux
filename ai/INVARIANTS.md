# Invariants (AI + Reviewer Reference)

Keep this file short and operational.
If a change breaks or modifies an invariant, update this file in the same commit.

## Port Table / Slots

- `Port_Table.lock` protects all table mutations (`slots`, `ht`, `live_ports`, freelist).
- `port_slots_lookup/resolve` success returns with one valid ref (`ref_get_not_zero`).
- `port_slots_free_slot` must bump `slot.gen` before slot reuse.
- `PORT_TABLE_SLOT_TOKEN_INVALID` must be type-consistent with `slot_index` width.
- `unregister` path must: remove hash mapping, unlink port, clear slot, decrement live count, free slot.
- `fini` must not leave registered ports silently alive.
- Rehash must be two-phase: build new table fully, then swap.

## Thread Port Cache

- Cache entry occupancy is determined by valid token state, not by stale string fields.
- Hash collision must not cause false-negative early return.
- Failed resolve of a candidate entry invalidates that entry only.
- Cache does not own a persistent port ref; each lookup/resolve acquires its own ref.

## Allocator Ownership

- Table-internal buffers (`slots`, `ht`) are allocated/freed by the same table allocator.
- Destroy path must keep allocator valid until table object free completes.

## Maintenance Rule

- Any bug that reveals a missing invariant here must add a new bullet in the same fix commit.

