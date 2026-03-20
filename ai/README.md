# AI Collaboration Docs

This folder stores persistent AI collaboration artifacts for the whole repository.

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

1. Implement code changes.
2. Run review using `AI_CHECKLIST.md`.
3. Before commit, append one entry to `ASSIST_HISTORY.md`.
4. If a new bug pattern appears, update `AI_CHECKLIST.md` in the same commit.
5. If history exceeds rotation threshold, archive old entries per `ARCHIVE_POLICY.md`.

## Goal

Keep context durable across model context resets/restarts and reduce repeated failures.

