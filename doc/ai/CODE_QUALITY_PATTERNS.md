# Code Quality Patterns

Abstract patterns and principles for high-quality kernel code, distilled from real optimization and design discussions.

## Philosophy

**"可预测性" (Predictability) > 简洁性**

代码质量的核心不是写出最短的代码，而是让系统的行为可预测：
- **可预测的性能**：没有隐藏的性能陷阱
- **可预测的状态**：要么成功，要么完全回滚
- **可预测的类型**：正确的类型，不需要强制转换
- **可预测的清理**：对称的获取和释放

---

## Pattern 1: Performance Optimization "Three Questions"

Before optimizing any code, ask three questions:

1. **Does this computation really need to be repeated?**
   - Example: `have_mapped` called twice → cache result in union field
   - Check: Is the result stable between calls? Can we cache it?

2. **Does this lookup really need to happen?**
   - Example: Red-black tree search for `vs->_vspace_node` → direct field access
   - Check: Is the reference stable? Can we access it directly?

3. **Does this traversal really need to happen?**
   - Example: Three-phase algorithm → two-phase (merge collection+validation)
   - Check: Can we merge phases? Can we skip large boundaries?

**Key principle**: Each computation/lookup/traversal must have clear value.

---

## Pattern 2: Field Repurposing "Four Safety Elements"

Before repurposing any struct field as temporary cache:

1. **Who uses it originally?**
   - Example: `manage_free_list` only used by manager nodes
   - Check: grep for all usage sites, understand the semantic

2. **When is it safe to repurpose?**
   - Example: vspace lock held, no concurrent `is_page_manage_node()` checks
   - Check: What code runs concurrently? What locks protect access?

3. **How to restore it?**
   - Example: Set both fields to 0 (not INIT_LIST_HEAD) for NULL checks
   - Check: Does `is_page_manage_node` check `next && prev`? Use appropriate cleanup.

4. **How to verify the restoration?**
   - Example: All fields restored to 0 before lock release
   - Check: Are there any observers that could see inconsistent state?

**Key principle**: Field repurposing is not a hack—it requires complete safety analysis.

**Union design pattern**:
```c
union {
    struct list_entry manage_free_list;  // Scenario A: manager nodes
    struct {
        ppn_t cached_ppn;               // Scenario B: performance cache
        ENTRY_FLAGS_t cached_flags;     // Scenario B: rollback support
    } cache_data;
};
```

---

## Pattern 3: Naming Semantic Precision

Names should express **essential purpose**, not **current usage**:

- **Wrong**: `_free_list` →暗示单一用途（free list），限制复用
- **Correct**: `aux_list` →表达本质（auxiliary），支持多场景

**Rule**: If a name suggests a specific usage but serves a general purpose, rename it.

---

## Pattern 4: Function Design "Parametric Evolution"

Functions evolve naturally through three stages:

```c
// Stage 1: Duplicate code
while (!list_empty(&list1)) { /* cleanup */ }
while (!list_empty(&list2)) { /* cleanup */ }

// Stage 2: Extract function
static inline void cleanup_list(struct list_entry* list) { ... }

// Stage 3: Parameterize differences
static inline void cleanup_list(struct list_entry* list,
                                bool delete_nodes) {
    if (delete_nodes)
        delete_nexus_entry(...);
}
```

**Key principle**: Recognize code patterns and extract reusable functions.

---

## Pattern 5: Type "Zero-Force-Cast" Principle

**Never use forced type conversions in union design**:

```c
// ❌ Type unsafe (forces casts)
struct {
    struct list_entry* cached_ppn;      // Wrong type!
    struct list_entry* cached_flags;    // Wrong type!
} cache_data;
node->cached_ppn = (struct list_entry*)ppn;  // Ugly! Unsafe!

// ✅ Type safe (correct types)
struct {
    ppn_t cached_ppn;           // Correct type!
    ENTRY_FLAGS_t cached_flags; // Correct type!
} cache_data;
node->cached_ppn = ppn;  // Clean! Safe!
```

**Rule**: If you need forced type conversion, the design is wrong.

---

## Pattern 6: Atomicity "All-or-Nothing"

**Partial failure is not acceptable**—implement full rollback:

```c
// ❌ Partial update failure → inconsistent state
Node 1: ✅ Success (flags modified)
Node 2: ✅ Success (flags modified)
Node 3: ❌ Failure
Result: System state inconsistent!

// ✅ Full rollback → consistent state
Node 1: ✅ Success → Rollback on failure
Node 2: ✅ Success → Rollback on failure
Node 3: ❌ Failure → Triggers rollback
Result: All nodes restored to original state
```

**Implementation pattern**:
1. **Phase 1**: Collect nodes, cache original state
2. **Phase 2**: Batch update, track success count
3. **Rollback**: If any fail, restore all updated nodes

**Key principle**: Atomicity means "either all succeed, or none happen".

---

## Pattern 7: Union Design "Type-Safe Expression"

Union members should clearly express their distinct purposes:

```c
union {
    struct list_entry manage_free_list;  // Manager node usage
    struct {
        ppn_t cached_ppn;               // Performance cache
        ENTRY_FLAGS_t cached_flags;     // Rollback support
    } cache_data;
};
```

**Benefits**:
- **Type safety**: Compiler checks types, no forced conversions
- **Self-documenting**: Member names express usage intent
- **Memory layout**: Same size (128 bits on 64-bit), no overhead

**Rule**: Union should clarify usage, not obscure it.

---

## Pattern 8: Comments "Standard Format"

Use **Doxygen-style** documentation consistently:

```c
/*
 * @brief One-line summary of function purpose
 *
 * @param param1: description
 * @param param2: description
 * @return type: return value description
 * @note Important constraints or side effects
 */
error_t function_name(type1 param1, type2 param2);
```

**Key principle**: Comments are standardized documentation, not free-form text.

---

## Pattern 9: Error Handling "Symmetric Cleanup"

Every resource acquisition must have symmetric release:

```c
// ✅ Symmetric cleanup
ret = REND_SUCCESS;  // Initial state
// ... operations ...
if (error) {
    ret = ERROR_CODE;  // Set error
    goto cleanup;      // Unified cleanup
}
cleanup:
    cleanup_aux_list(...);  // All paths converge
    return ret;            // Clear return value
```

**Rule**: Error paths should not bypass cleanup logic.

---

## Pattern 10: Code Optimization "Progressive Refinement"

Code quality improves through multiple iterations:

1. **Round 1**: Correctness (basic implementation)
2. **Round 2**: Performance (eliminate redundant work)
3. **Round 3**: Readability (unified cleanup, extract helpers)
4. **Round 4**: Type safety (union + correct types)
5. **Round 5**: Atomicity (full rollback support)
6. **Round 6**: Documentation (Doxygen comments)

**Key principle**: Quality is iterative, not one-shot.

---

## Meta-Rule: Abstraction Over Specificity

**Abstract patterns over specific rules**:

- ❌ Bad: "nexus nodes must use cached_ppn field"
- ✅ Good: "Union field repurposing requires safety analysis and documentation"

- ❌ Bad: "Use goto for cleanup in all functions"
- ✅ Good: "Error handling requires symmetric cleanup and unified exit"

**Rule**: One general rule covering many cases > many narrow rules.

---

## How to Use This Document

1. **Before implementing**: Review relevant patterns to avoid common pitfalls
2. **During code review**: Check if code follows these patterns
3. **After discovering bugs**: Extract abstract pattern and add to this document
4. **When updating patterns**: Modify this document (not AI_CHECKLIST.md) if pattern changes

---

## Related Documentation

- `AI_CHECKLIST.md`: Mandatory review checklist (checkable abstract patterns)
- `INVARIANTS.md`: Runtime/design invariants (system truths)
- `ARCHITECTURE.md`: High-level design principles
- `DECISIONS.md`: Architecture decision records (ADR-lite)

---

## Pattern Log (append-only)

- 2026-04-16: Initial extraction from nexus_update_range_flags optimization:
  - Field repurposing with union type safety
  - Performance optimization three questions
  - Full rollback atomicity pattern
  - Direct struct reference vs tree lookup
  - Function parametric evolution
  - Type zero-force-cast principle
  - Progressive code refinement
