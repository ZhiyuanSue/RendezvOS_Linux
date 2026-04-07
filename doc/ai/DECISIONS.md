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

## 2026-04 | Generic string-keyed index named `name_index` (not “named_slot_table”)

- Context: the reusable backend is “index string name → stored value (+ row generation for cache tokens)”; the old name sounded like an implementation detail (“slot”) rather than the role.
- Decision: expose it as `name_index` (`name_index_t`, `name_index_token_t` with `row_index` / `row_gen`); port table embeds `name_index_t by_name`.
- Consequences:
  - clearer mental model at call sites; internal storage remains a row array + open-addressing hash.
  - `name_index_token_t` stays as a typedef alias for `name_index_token_t` (IPC-facing name unchanged).

## 2026-03 | Thread cache stores hash + token, no name copy

- Context: avoid duplicate string compare/storage in thread cache entries.
- Decision: remove `name_copy`; lookup resolves all same-hash candidates via token.
- Consequences:
  - saves per-thread cache memory.
  - collision handling relies on trying all same-hash candidates.

## 2026-03 | Kernel IPC messages use a common envelope (`kmsg`)

- Context: multiple in-kernel server threads communicate via lock-free IPC; ad-hoc per-service payload structs make validation/versioning hard.
- Decision: reserve a kernel message `msg_type` and wrap payloads in a versioned `kmsg` header with `kind + payload_len`.
- Consequences:
  - message receivers can validate `magic/version/len` before casting.
  - new services add new `kind` values without changing the base IPC layer.

## 2026-03 | 64-bit index/capacity/token fields in port table path

- Context: target architectures are 64-bit; alignment/consistency favored.
- Decision: use 64-bit widths for slot index/capacity/token index on port table path.
- Consequences:
  - more uniform type model and fewer width-conversion footguns.
  - hash bucket storage uses more memory than 32-bit variant.

---
## 2026-03 | When to forward work to server threads

- Context: core provides primitives (thread/memory/IPC/page-table basics), but certain “business semantics + resource ownership / lifecycle constraints” must not be executed directly on the caller stack without additional safety.
- Decision: forward work to a top-level server thread only if at least one condition holds:
  - caller context resources may be unavailable (example: `exit` teardown must run after switching to a safe vspace/stack).
  - strict serialization is required for a shared/unique resource (example: network/NIC queue ownership).
  - complex multi-step rollback/lifecycle correctness is better centralized into a single executor.
- Consequences:
  - core stays focused on primitives with stronger low-level invariants.
  - server threads become the place for strong ordering and lifecycle correctness.

---
## 2026-03 | Current user mappings are 4K-only (so COW targets 4K first)

- Context: nexus 用户态分配路径在 `_user_take_range()` 固定调用 `_take_range(false, ...)` 并且 `user_fill_range()` 只 `map(..., level=3)`；当前未维护用户态 2M/huge 节点数据结构。
- Decision: implement fork+COW minimal closure for 4K first; extend to 2M/huge only after the user-side 2M mapping data-path is introduced.
- Consequences:
  - avoids unnecessary huge-page COW complexity early.
  - keeps page-table teardown/reclamation scoped to the common 4K path.

---
## 2026-03 | Hybrid kernel: core primitives + server threads via IPC

- Context:
  - `core/` implements the minimal kernel core: scheduling/thread management, memory primitives (PMM/Nexus/map/unmap), and fast IPC/port-based object lookup.
  - Higher-level “Linux compatibility” and syscall semantics are implemented as in-kernel `servers/` (and other kernel-mode services).
  - Syscalls in `linux_layer/` typically do not execute heavy cleanup directly; they send requests via IPC ports to the corresponding server thread.
  - Some teardown actions (e.g. `exit` cleanup) must be performed outside the caller’s unsafe execution context (stack/vspace hazards), similar to the motivation behind `clean_server`.
- Decision:
  - Keep `core/` as a set of reusable, tightly-scoped primitives with strong lifetime/teardown invariants.
  - Centralize business semantics and resource-ownership ordering into server threads that operate through IPC message passing.
  - Use server forwarding when (1) the caller context may be invalid or unsafe for the action, or (2) serialization over a shared/unique resource is required.
- Consequences:
  - Pros:
    - Clear layering: fewer places where low-level memory/page-table invariants are broken by high-level syscall semantics.
    - Stronger lifetime correctness: teardown and refcount ownership can be centralized and audited in one service.
    - Better performance predictability: hot IPC/data paths stay in `core`, while expensive/complex operations move to servers.
    - Matches your “one syscall service thread owns the hard part” design, reducing multi-core contention on unique resources.
  - Cons / risks:
    - Requires very explicit core<->server contracts (what core guarantees vs what server must guarantee).
    - Teardown correctness becomes a cross-module invariant; missing a single reclamation step (e.g. vspace page-table frames) causes leaks.
    - Debugging complexity increases: a failure can manifest far from the code that triggered it (via message passing).
    - Future features like `fork+COW` increase the surface area of correctness: page refcounting and rmap coherence must remain consistent under server-driven lifetimes.
  - Targeted improvements for future work:
    - Define a small set of “ownership + teardown” invariants that every server must respect when driving `core` primitives.
    - For fork/COW, formalize how nexus nodes and rmap entries are duplicated so exit cleanup never mis-frees shared physical pages.
    - Expand tests to cover server-driven teardown paths and memory reclamation regressions under concurrent workloads.

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
