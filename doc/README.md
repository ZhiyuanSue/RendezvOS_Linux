# RendezvOS_Linux — documentation map

**Single entry point** for repository-wide documentation.

---

## Three domains

| Domain | Start here | Scope |
|--------|------------|--------|
| **Using core** | [`core/docs/USING_CORE.md`](../core/docs/USING_CORE.md) | Public APIs, call patterns, MM/IPC/trap usage — **all “how to use core” docs live under `core/docs/`** |
| **Linux compat** | [`linux_compat/README.md`](linux_compat/README.md) | Syscall semantics, data model, VFS, signals |
| **AI / process** | [`ai/README.md`](ai/README.md) | Invariants, checklist, IPC wire format, tests |

**Rule:** `core/docs/` does not describe the compat layer. Compat docs do **not** duplicate core usage guides.

---

## I work on…

| Task | Read |
|------|------|
| Anything that **calls core APIs** | [`core/docs/USING_CORE.md`](../core/docs/USING_CORE.md) |
| Linux **syscall / signal / VFS** policy | [`linux_compat/README.md`](linux_compat/README.md) canonical table |
| **AI-assisted change** | [`ai/AI_CHECKLIST.md`](ai/AI_CHECKLIST.md) + [`ai/INVARIANTS.md`](ai/INVARIANTS.md) |
| **Before commit** | [`ai/TEST_MATRIX.md`](ai/TEST_MATRIX.md) |

---

## Linux compat — canonical

See [`linux_compat/README.md`](linux_compat/README.md). Archived drafts: [`linux_compat/archive/README.md`](linux_compat/archive/README.md).

---

## AI / cross-cutting

| Document | Role |
|----------|------|
| [`INVARIANTS.md`](ai/INVARIANTS.md) | Runtime rules |
| [`AI_CHECKLIST.md`](ai/AI_CHECKLIST.md) | Review patterns |
| [`IPC_MESSAGE.md`](ai/IPC_MESSAGE.md) | kmsg / TLV / reply port `t` |
| [`DECISIONS.md`](ai/DECISIONS.md) | ADR-lite |

Stale core API drafts: [`ai/archive/core_api_stale/README.md`](ai/archive/core_api_stale/README.md).

---

## Maintenance

1. **Core usage** → only [`core/docs/USING_CORE.md`](../core/docs/USING_CORE.md) + topic files; update there.
2. **Compat policy** → `linux_compat/` canonical table.
3. **Core code change** → `core/docs/GUIDE.md` §6–§7 + `USING_CORE.md` if patterns change.
4. New failure pattern → `AI_CHECKLIST.md` with the fix.

Workflow: [`ai/README.md`](ai/README.md) · [`AGENTS.md`](../AGENTS.md) · [`CLAUDE.md`](../CLAUDE.md).
