# Decisions (ADR-lite)

Short log of important decisions to avoid re-litigating old design choices.
Format: Context / Decision / Consequences.

---

## 2026-03 | Port table backend uses slots + open-addressing hash

- Context: read-mostly IPC port lookup; RB-based path was removed.
- Decision: use dynamic slot array + OA hash + per-slot generation token.
- Consequences:
  - simpler hot lookup path and token-based cache resolve.
  - requires careful rehash rollback discipline and teardown handling.

## 2026-03 | Thread cache stores hash + token, no name copy

- Context: avoid duplicate string compare/storage in thread cache entries.
- Decision: remove `name_copy`; lookup resolves all same-hash candidates via token.
- Consequences:
  - saves per-thread cache memory.
  - collision handling relies on trying all same-hash candidates.

## 2026-03 | 64-bit index/capacity/token fields in port table path

- Context: target architectures are 64-bit; alignment/consistency favored.
- Decision: use 64-bit widths for slot index/capacity/token index on port table path.
- Consequences:
  - more uniform type model and fewer width-conversion footguns.
  - hash bucket storage uses more memory than 32-bit variant.

---

## Add New Decision Template

```md
## YYYY-MM | <title>

- Context: <what pressure/problem existed>
- Decision: <what was chosen>
- Consequences:
  - <good>
  - <trade-off/risk>
```

