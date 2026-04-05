# AI Collaboration Docs

This folder stores persistent AI collaboration artifacts for the whole repository (under `doc/ai/`).

**Discovery:** the repo root `AGENTS.md` points Cursor Agent here; keep that file short and link-focused so this folder stays canonical.

## Files

- `AI_CHECKLIST.md`: mandatory review checklist for AI-assisted changes.
- `ASSIST_HISTORY.md`: append-only change history for AI-assisted commits.
- `ARCHIVE_POLICY.md`: rotation/archiving rules for history files.
- `INVARIANTS.md`: runtime/design invariants that must stay true.
- `DECISIONS.md`: short architecture/design decision log (ADR-lite).
- `TEST_MATRIX.md`: minimum tests by change type.
- `RED_TEAM_REVIEW.md`: adversarial review checklist for regression hunting.
- `archive/`: monthly archived history files.

## Workflow

1. Implement or propose code changes (prefer **local edits + diff for review**; the result is not “correct” until verified or reviewed).
2. Run review using `AI_CHECKLIST.md` (and `TEST_MATRIX.md` when applicable).
3. **Verification gate:** record what was run (`make …`, tests, boot) or explicitly **not run** (toolchain missing, etc.). Do **not** imply correctness without verification or maintainer review.
4. **Commit gate:** **do not `git commit` unless the user explicitly asked to commit** or confirmed the batch is ready. If the user asked only for analysis, a plan, or a patch to try, leave changes **uncommitted** or deliver a diff/summary for them to apply and test.
5. When the user **does** approve a commit: append one entry to `ASSIST_HISTORY.md` **in that commit** (or immediately after, per team habit), with the verification field accurate.
6. If a new bug pattern appears, update `AI_CHECKLIST.md` (including the **Pattern Log** section) in the **same commit** as the fix—do **not** introduce a separate parallel “rules” file for every lesson unless no existing checklist section can hold it.
7. If history exceeds rotation threshold, archive old entries per `ARCHIVE_POLICY.md`.

## Rules for assistants (read first)

- Treat `AI_CHECKLIST.md` as the single authoritative place for **checkable** patterns (including naming/identifier discipline). Prefer **one abstract rule** over many file-specific examples.
- **Checklist hygiene (§0):** when a lesson applies broadly (e.g. naming), fold it into **one** checkable pattern (role, symmetry, ownership)—cite in-repo code as **examples only**. Do **not** expand `AI_CHECKLIST.md` into a growing table of mandatory identifier spellings; that fights the meta-rule “prefer 1 general rule over many narrow rules.”
- Keep `INVARIANTS.md` for **runtime/design** truths; cross-reference the checklist for auditability/naming, do not duplicate long explanations there.
- After substantive AI-assisted work, the user expects checklist/history updates **before** the change is considered “closed,” not a one-off doc dropped elsewhere.
- **Commits:** assistants must not push or commit by default without explicit maintainer approval; verification and review come first (see Workflow §3–4).

## Goal

Keep context durable across model context resets/restarts and reduce repeated failures.

