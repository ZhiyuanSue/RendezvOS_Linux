# Red-Team Review (AI Adversarial Pass)

Run this as a second pass after implementation.
Goal: find breakage, not explain design.

## How to Use

Ask AI to review with this exact priority:

1. correctness bugs
2. race/lifetime regressions
3. rollback/teardown inconsistencies
4. missing tests for touched risk areas

No style-only comments until correctness risks are exhausted.

## Required Review Output

- Findings ordered by severity:
  - Critical / High / Medium
- Each finding includes:
  - location (`path` + symbol/function)
  - trigger condition
  - expected vs actual behavior
  - suggested minimal fix
- Then:
  - assumptions/open questions
  - test gaps

## High-Risk Prompts (copy/paste)

- "Assume one allocation fails at each step. Show if state remains consistent."
- "Assume token is stale and slot reused. Can resolve return wrong object?"
- "Assume hash collision under cache lookup. Any false-negative/false-positive path?"
- "Assume teardown starts with non-empty table. Are refs and indices drained safely?"
- "Assume rehash fails mid-way. Is old table still fully valid?"

## Stop-Ship Conditions

Any of the following should block merge until fixed or explicitly accepted:

- inconsistent state after failure return
- possible use-after-free / double free
- missing ref acquisition/release on success/failure path
- destroy/fini path that can silently leak registered/live objects
- signature mismatch between header and implementation

