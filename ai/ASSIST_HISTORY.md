# AI Assist History

Purpose: durable, compact context of AI-assisted changes across long sessions.
Append one entry for each user-approved commit.

## Archives

- No archive yet.
- When rotation starts, list files here, e.g.
  - `ai/archive/ASSIST_HISTORY_2026-03.md`

## Entry Rules (Mandatory)

- Append-only; do not rewrite old entries except factual corrections.
- One entry per commit (or tightly coupled commit batch).
- Keep each entry concise (target: 10-30 lines).
- If a new bug pattern was discovered, include `Pattern:` and confirm checklist update.
- If entries become too large:
  - keep this file as rolling index for recent 30-50 entries
  - move older entries to `ai/archive/ASSIST_HISTORY_YYYY-MM.md` and leave links

## Entry Template

```md
## YYYY-MM-DD | <short title> | commit <sha>

- Scope: <modules/files touched>
- Why: <problem and motivation>
- Design decision(s):
  - <key trade-off 1>
  - <key trade-off 2>
- Data structure/API impact:
  - <public API change or "none">
  - <invariant changes>
- Failure-path strategy:
  - <path>: <strategy> -> <post-condition>
- Verification:
  - <lint/build/tests run>
  - <known gaps>
- Pattern: <new bug pattern, optional>
- Checklist update: <yes/no + section>
```

---

## 2026-03-20 | Port table slots hardening | commit <pending>

- Scope: `core/kernel/task/port_table_slots.c`, `core/kernel/task/port.c`,
  `core/include/rendezvos/task/port.h`, `core/include/rendezvos/task/tcb.h`,
  `core/kernel/task/thread.c`.
- Why: stabilize slot+hash backend, tighten lifecycle/teardown safety, improve cache memory/use pattern.
- Design decision(s):
  - keep global lock + OA hash for read-mostly port lookup workload.
  - use token `(slot_index, slot_gen)` for cache resolve and ABA resistance.
  - remove per-entry `name_copy` in thread cache; resolve by hash candidates + token validation.
- Data structure/API impact:
  - port table indices/capacities/token index use 64-bit widths.
  - thread cache entry removed `name_copy`; now stores `name_hash + slot_tok`.
- Failure-path strategy:
  - rehash build failure: rollback (old hash remains valid; new hash freed).
  - register partial failure (slot allocated, ht insert failed): rollback slot to freelist.
  - fini on non-empty table: drain (unregister + ref_put until empty).
- Verification:
  - lints on touched files clean.
  - runtime tests not executed in this turn (needs local run).
- Pattern:
  - rehash swap-before-build bug class.
  - allocator-lifetime in fini/destroy bug class.
- Checklist update: yes (`AI_CHECKLIST.md`: Failure/Rollback + Teardown sections).

