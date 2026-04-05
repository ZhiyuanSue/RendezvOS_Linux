# Agent instructions (RendezvOS_Linux)

Use **Chat / Agent** with this repo. For full workflow and file index, read:

@doc/ai/README.md

Before treating a change as done:

1. Review against @doc/ai/AI_CHECKLIST.md (especially §0 meta-rules and §7 API/type discipline).
2. **Verification:** run build/tests when possible, or state what was **not** run—do not claim correctness without evidence or maintainer review (see @doc/ai/README.md workflow).
3. **Commit:** do not commit unless the maintainer asked for a commit or confirmed the batch; assistants default to **uncommitted** changes + diff for review.
4. New bug patterns: update that checklist **and** its **Pattern Log** in the **same** commit as the fix.
5. On an approved commit: append one entry to @doc/ai/ASSIST_HISTORY.md (see template inside).

Do not duplicate long policy here; keep `doc/ai/` as the canonical source.
