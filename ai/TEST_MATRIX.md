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

