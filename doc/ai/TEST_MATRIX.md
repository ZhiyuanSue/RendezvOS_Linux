# Test Matrix (Minimum Verification by Change Type)

Use this matrix to decide the minimum tests before merge.
If blocked, state blocker explicitly in review output/history.

## A) Port Table / IPC Lookup Path Changes

- Minimum:
  - build/lint for changed files
  - `smp_port_robustness_test` (focus on register/unregister + lookup/resolve + mixed ops)
- Should also run:
  - `single_port_test`
  - `single_ipc_test` (sanity of IPC path interaction)

## B) Cache/Token Logic Changes (`thread_lookup_port`, cache entry layout)

- Minimum:
  - build/lint
  - robustness test phase that stresses cache hit/miss/collision behavior
- Should also run:
  - mixed parallel operations test (if available)
  - repeated register/unregister + lookup stress

## C) Rehash/Grow/Freelist/Teardown Changes

- Minimum:
  - build/lint
  - stress with growth + unregister churn
  - destroy/fini path with non-empty table scenario
- Required checks:
  - no stale lookup after unregister
  - no crash on teardown

## D) Refcount/Lifetime Changes

- Minimum:
  - build/lint
  - stress test with frequent lookup/unregister/free
- Required checks:
  - no double free / no use-after-free symptoms
  - no leaked live refs after teardown

## E) Type-width/ABI/Struct Layout Changes

- Minimum:
  - build/lint on target arch config(s)
  - runtime smoke on at least one target setup
- Required checks:
  - sentinel comparisons still valid
  - no signature mismatch across headers/sources

## F) MM / Map / Page-table–adjacent API Changes

- Minimum:
  - build/lint for changed translation units
- Required checks:
  - `AI_CHECKLIST.md` §7: no parameter/local shadowing of typenames used in
    casts or macro arguments; address role (kernel vs user VA, etc.) still obvious
    from names or comments.

---

## Result Recording Template

```md
- Tests run:
  - <name>: pass/fail/blocked
- Blockers:
  - <reason>
- Residual risk:
  - <what still not covered>
```

## G) User Payload Test Changes

- Minimum:
  - **qemu-user validation**: All test cases MUST be validated on qemu-user BEFORE
    testing on RendezvOS kernel
  - Required steps:
    1. Build static-linked test binary
    2. Run on appropriate qemu-user (e.g., `qemu-aarch64-static`)
    3. Only test on kernel after qemu-user validation passes
  - Required checks:
  - No test debugging on kernel unless test passes on qemu-user
  - Verify test logic is correct on standard Linux first
- Rationale:
  - Distinguishes test bugs from kernel implementation bugs
  - Prevents wasting time debugging test case issues
  - Ensures tests have correct expected behavior

## H) Linux compat proc/signal/wait changes

- Minimum:
  - **Paired** `make ARCH=x86_64 run` and `make ARCH=aarch64 run`
  - Record in [`doc/linux_compat/CROSS_ARCH_VERIFICATION_LOG.md`](../linux_compat/CROSS_ARCH_VERIFICATION_LOG.md)
- Required checks (see log for grep targets):
  - Harness 52/52 both arches
  - #44 `PASS: SIG_IGN still works`; no `[SIGNAL] deliver: invalid user handler`
  - #49 `[TEST 49/52] PASS` **after** `=== Test Summary ===`
  - #49 multiple children: exit_code 10, 20, 30
  - Fork/clone diff: child `test_cookie == 0`

## I) Linux compat time syscall changes

- Minimum:
  - Paired harness run after `gettimeofday` / `nanosleep` / `times` changes
  - Record in [`doc/linux_compat/CROSS_ARCH_VERIFICATION_LOG.md`](../linux_compat/CROSS_ARCH_VERIFICATION_LOG.md)
- Required checks:
  - #16 no `gettimeofday ... not implemented`
  - #17 no `unimplemented id=96` / Assert Fatal from sleep test
  - #20 no `times ... not implemented`
- Plan: [`TIME_SUBSYSTEM_PLAN.md`](../linux_compat/TIME_SUBSYSTEM_PLAN.md)

### See Also
- `user_payload/README.md` - Detailed validation guidelines and scripts

