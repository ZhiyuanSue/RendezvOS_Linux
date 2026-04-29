# AI Collaboration Docs

This folder stores persistent AI collaboration artifacts for the whole repository (under `doc/ai/`).

**Discovery:** the repo root `AGENTS.md` points Cursor Agent here; keep that file short and link-focused so this folder stays canonical.

## Files

- `AI_CHECKLIST.md`: mandatory review checklist for AI-assisted changes.
- `CODE_QUALITY_PATTERNS.md`: abstract patterns distilled from real optimizations (patterns > specific rules).
- `ASSIST_HISTORY.md`: append-only change history for AI-assisted commits.
- `ARCHIVE_POLICY.md`: rotation/archiving rules for history files.
- `INVARIANTS.md`: runtime/design invariants that must stay true.
- `DECISIONS.md`: short architecture/design decision log (ADR-lite).
- `TEST_MATRIX.md`: minimum tests by change type.
- `RED_TEAM_REVIEW.md`: adversarial review checklist for regression hunting.
- `NEXUS_API_FREEZE.md`: temporary frozen nexus APIs/flags during big refactors.
- `archive/`: monthly archived history files.

## Workflow

**Standard workflow (linux_layer/, servers/, tests/)**:
1. Implement or propose code changes (prefer **local edits + diff for review**).
2. Run review using `AI_CHECKLIST.md` (and `TEST_MATRIX.md` when applicable).
3. **Verification gate:** record what was run (`make …`, tests, boot) or explicitly **not run**.
4. **Commit gate:** leave changes **uncommitted** unless user explicitly approves.
5. When user **does** approve a commit: append one entry to `ASSIST_HISTORY.md`.
6. If a new bug pattern appears, update `AI_CHECKLIST.md` in the **same commit**.

**Special workflow (core/ changes)**:
1. **Stop immediately** if AI discovers need to modify core/.
2. **Propose change:** explain what/why/how with design alternatives.
3. **Wait for user approval** before implementing.
4. **User review:** user reviews, possibly iterates with AI.
5. **User commits:** only user commits core/ changes, not AI.

**Rationale:** core/ contains high-quality, iteratively-optimized code. AI assists in linux_layer/ to verify rapid Linux compatibility layer construction on top of core/ framework.

## Rules for assistants (read first)

**Core constraints**:
- **core/ modification ban:** AI must not modify core/ code without explicit user approval. Propose changes first, wait for approval, then implement.
- **Abstraction over specificity:** Prefer one general rule covering many cases over many narrow rules.
- **Checklist hygiene:** When a lesson applies broadly (e.g. naming), fold it into **one abstract pattern** (role, symmetry, ownership). Use in-repo code as **examples only**.

**Documentation discipline**:
- `AI_CHECKLIST.md`: single authoritative place for **checkable abstract patterns**.
- `INVARIANTS.md`: **runtime/design** truths only. Cross-reference checklist, don't duplicate.
- After substantive AI-assisted work, update checklist/history **before** considering the change "closed".

**Verification and commits**:
- Do not claim correctness without verification or maintainer review.
- Leave changes uncommitted by default; only commit when user explicitly approves.

## Goal

**Primary objective:** Verify that AI can rapidly construct a Linux compatibility layer (200-300+ syscalls, multi-arch) on top of the core/ framework while maintaining code quality and architectural coherence.

**Documentation purpose:** Keep context durable across model context resets/restarts and reduce repeated failures by maintaining abstract patterns and verified lessons learned.

