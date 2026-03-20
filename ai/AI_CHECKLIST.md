# AI Change Checklist (Repository-wide)

This checklist is for AI-assisted code changes in this repository.
Primary target: prevent hidden regressions in concurrency, lifetime, teardown, and failure paths.

## Required Output Per Change

Before merge/commit, include a short note with:

1. **Invariants** (3-8 bullets)
2. **Failure paths** (alloc fail, partial update fail, concurrent invalidation)
3. **Lock/refcount order** (under lock vs after unlock)
4. **Teardown behavior** (require-empty vs drain-on-fini)
5. **Verification** (lint/build/test or explicit blocker)

If any section is missing, review is incomplete.

---

## Core Checklist

### 1) Data Structure Invariants

- [ ] State fields have clear meaning (`used`, `gen`, `free_head`, `live_count`).
- [ ] Sentinel values are type-consistent (`U64_MAX` for `u64` index invalid).
- [ ] Capacity/index arithmetic has overflow checks before allocation math.
- [ ] Token/cached handle invalidation is explicit (e.g., generation bump on free).

### 2) Concurrency and Locking

- [ ] Shared mutations happen under the intended lock.
- [ ] Lock order is unchanged or explicitly documented when changed.
- [ ] No lockless access assumes stability unless guaranteed by design.
- [ ] Potentially heavy free paths run outside lock when possible.

### 3) Refcount and Lifetime

- [ ] Lookup/resolve acquires a valid ref on success.
- [ ] Remove/unregister drops ownership ref exactly once.
- [ ] Destroy/fini does not silently leak live objects.
- [ ] Final free path does not mutate index structures unexpectedly.

### 4) Failure and Rollback

- [ ] Every allocation failure has deterministic behavior.
- [ ] Partial updates are fully rolled back or fully committed.
- [ ] Rehash/grow uses two-phase commit:
  - build new structure first
  - swap pointers only after full success
- [ ] On failure, old structure remains valid and unchanged.

### 5) Lookup/Cache Correctness

- [ ] Hash collision cannot cause false-positive return.
- [ ] Hash collision cannot cause false-negative early return.
- [ ] Stale token/generation mismatch invalidates cache entry correctly.
- [ ] Cache replacement/eviction keeps occupancy/count consistent.

### 6) Teardown and Allocator Ownership

- [ ] `fini` policy is explicit: require empty or drain.
- [ ] Allocator ownership is consistent across alloc/grow/free/free(table).
- [ ] Destroy path does not null allocator before final free(table).

### 7) API/Type Discipline

- [ ] Header/source signatures match exactly.
- [ ] Type width choices are intentional for target architectures.
- [ ] Comments match actual behavior (no stale comment drift).

### 8) Validation

- [ ] Lints checked for modified files.
- [ ] Build/tests run where possible (or explicit blocker stated).
- [ ] Residual risks listed if verification is partial.

---

## Failure Path Strategy Template (Mandatory)

For each meaningful failure path, choose one strategy:

- **Rollback**: restore previous consistent state, return error.
- **Fail-fast**: stop, return error, no state mutation.
- **Drain/Cleanup**: continue safe cleanup, then return status.
- **Panic/Bug**: only for impossible corruption states.

Template:

```
Path: <function + failure point>
Strategy: <Rollback/Fail-fast/Drain/Panic>
Reason: <1-2 lines>
Post-condition: <what remains valid>
```

---

## Mandatory Update Mechanism

When a new bug pattern appears during review/debug:

1. Update this checklist in the same commit.
2. Append a new pattern entry in this file.
3. Append a short change summary in `ASSIST_HISTORY.md`.
4. Review is not complete unless steps 1-3 are done.

---

## Pattern Log (append-only)

- 2026-03: Rehash commit-order bug pattern:
  freeing/switching old table before full rebuild can leave inconsistent state on failure.
  Rule added under "Failure and Rollback" (two-phase rehash).

- 2026-03: Fini allocator lifetime bug pattern:
  nulling allocator in `fini` before final free(table) causes invalid free path.
  Rule added under "Teardown and Allocator Ownership".

