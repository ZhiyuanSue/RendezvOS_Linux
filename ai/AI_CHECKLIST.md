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

### 0) Abstraction / Non-Overfitting Policy (meta-rule)
- [ ] Each checklist entry describes a reusable *pattern* (symmetry, ownership, lock context, rollback strategy), not a one-off local implementation detail.
- [ ] If a rule mentions a concrete function/module name, it must be explicitly marked as an example, and the invariant/pattern must still be checkable without that name.
- [ ] Prefer 1 general rule that covers many cases over many narrow rules; if multiple bullets overlap, merge them and keep only the most actionable check.
- [ ] Every bullet must answer: "What can go wrong, and how do I check it?"
- [ ] When learning a new failure mode, update the closest existing category (invariants/teardown/failure/lookup/etc.) before adding a new category.

### 0b) Error-Reporting Layering (meta-rule)
- [ ] Lower layers return `error_t` (or equivalent status) and do not unconditionally print internal details.
- [ ] Higher layers decide whether/how to log (and can include context like request/thread/vspace ids).
- [ ] If an error is ignored on purpose, the code must document why and what remains safe.

### 0c) Low-Overhead Reviewability (meta-rule)
- [ ] Avoid extra temporary variables if the existing value can be reused for the decision and/or logging.
- [ ] Prefer linear control flow: do not refactor into additional scopes/identifiers without a correctness gain.

### 0d) Primary Error Preservation (meta-rule)
- [ ] If an earlier operation fails, do not overwrite its error status with later best-effort cleanup results.
- [ ] Cleanup errors must be either logged separately or aggregated, but the primary failure cause remains visible.

### 1) Data Structure Invariants

- [ ] State fields have clear meaning (`used`, `gen`, `free_head`, `live_count`).
- [ ] Sentinel values are type-consistent (`U64_MAX` for `u64` index invalid).
- [ ] Sentinel/constant names match their actual semantics (e.g., `free_head`
  sentinel is not reused as token/cache invalidation).
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
- [ ] Teardown must use the recorded owner metadata and the correct
  synchronization context (no "best-effort" fallbacks).
- [ ] If teardown relies on a per-CPU/per-thread scratch window (e.g.,
  self-mapping region used to edit frames), it must run in the context
  that owns that window.

### 7) API/Type Discipline

- [ ] Header/source signatures match exactly.
- [ ] Type width choices are intentional for target architectures.
- [ ] Comments match actual behavior (no stale comment drift).
- [ ] Naming consistency as an auditability constraint:
  - For symmetric / dual operations (e.g., alloc<->free, map<->unmap,
    enqueue<->dequeue, lock<->unlock), keep the same identifier vocabulary
    for shared concepts across the pair (e.g., `entry_flags`, `table`,
    `handler`, `lock`).
  - Avoid single-letter names for non-trivial struct members/fields.
    If a one-letter temporary is unavoidable, scope it tightly and add a
    short explanatory comment.

### 8) Validation

- [ ] Lints checked for modified files.
- [ ] Build/tests run where possible (or explicit blocker stated).
- [ ] Repo-wide usage checked for any symbol/function you remove (especially
  deprecated wrappers/macros) and public header API is preserved.
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

