# Radix Tree API 使用指南（Linux兼容层）

> **目标读者**：Linux兼容层开发者  
> **目的**：确保正确使用Radix Tree API，避免死锁和内存不一致

## 🚨 关键原则

### 1. 锁序规则（严格遵守）

**标准锁序**：L0 big lock → L2 band lock → PMM zone lock → vspace_lock  
**禁止逆序**：持PMM zone lock时不要取vspace_lock（可能死锁）

```c
// ✅ 正确的顺序
vmm_radix_tree_lock_range_big_and_small(handler, vs, start, end, RADIX_RL_INSERT);
// ... radix operations ...
vmm_radix_tree_unlock_range_big_and_small(vs, start, end);

// ❌ 错误：单独持zone锁时调用map/unmap
pmm_zone_lock(pmm->zone);
map(vs, ppn, vpn, 3, flags, handler);  // 可能死锁！
pmm_zone_unlock(pmm->zone);
```

### 2. Range API 使用流程

**分配路径**（mmap匿名页）：
```c
// 1. 计算区间结束地址
vaddr range_end;
if (!vmm_radix_tree_calculate_end_check(start, page_count, &range_end))
    return -EINVAL;

// 2. 持锁（INSERT模式）
error_t err = vmm_radix_tree_lock_range_big_and_small(
    handler, vs, start, range_end, RADIX_RL_INSERT);
if (err != REND_SUCCESS)
    return err;

// 3. 预留radix区间（LAZY）
err = vmm_radix_tree_insert_range(vs, owner, start, flags, range_end);
if (err != REND_SUCCESS) {
    vmm_radix_tree_unlock_range_big_and_small(vs, start, range_end);
    return err;
}

// 4. 分配物理页并映射页表
ppn_t ppn_first = pmm->pmm_alloc(pmm, page_count, &alloced);
for (int i = 0; i < page_count; i++) {
    err = map(vs, ppn_first + i, VPN(start + i * PAGE_SIZE), 
              3, flags, handler);
    if (err != REND_SUCCESS) {
        // 回滚：unmap已映射的前缀，然后释放锁
        // ...
    }
}

// 5. 绑定叶子（VALID + rmap）
err = vmm_radix_tree_leaf_bind_range(vs, start, ppn_first, 
                                      range_end, flags);
if (err != REND_SUCCESS) {
    // 回滚...
}

// 6. 释放锁
vmm_radix_tree_unlock_range_big_and_small(vs, start, range_end);
```

**释放路径**（munmap）：
```c
// 1. 计算区间结束地址
vaddr range_end;
if (!vmm_radix_tree_calculate_end_check(start, page_count, &range_end))
    return -EINVAL;

// 2. 持锁（QUERY_OR_CHANGE模式）
error_t err = vmm_radix_tree_lock_range_small_with_big_locked(
    handler, vs, start, range_end, RADIX_RL_QUERY_OR_CHANGE);

// 3. 解绑叶子（VALID → LAZY）
err = vmm_radix_tree_leaf_unbind_range(vs, start, ppn_first, range_end);

// 4. 释放L2锁（但保持L0锁）
vmm_radix_tree_unlock_range_small(vs, start, range_end);

// 5. 解映射页表
for (int i = 0; i < page_count; i++) {
    unmap(vs, VPN(start + i * PAGE_SIZE), 0, handler);
}

// 6. 重新持L2锁（DELETE模式）
err = vmm_radix_tree_lock_range_small_with_big_locked(
    handler, vs, start, range_end, RADIX_RL_DELETE);

// 7. 删除radix区间（DELETE工作在lock内部完成）
vmm_radix_tree_unlock_range_small(vs, start, range_end);

// 8. 释放物理页
pmm->pmm_free(pmm, ppn_first, page_count);
```

### 3. 使用编排层API（推荐）

直接使用 `mm_user_utils_*` 编排层API更简单：

```c
// 分配：PMM + radix + PTE + bind
vaddr addr = mm_user_utils_set_range_and_fill(
    vs, start, page_count, flags);
if (addr == 0)
    // 失败

// Lazy物化（page fault）
error_t err = mm_user_utils_fill_page_with_exist_range(
    vs, page_va, flags);

// 释放：unbind + unmap + delete + pmm_free
error_t err = mm_user_utils_clean_range_and_unfill(
    vs, start, page_count, ppn_first);

// COW remap
error_t err = mm_user_utils_remap_page(
    vs, page_va, new_ppn, new_flags, old_ppn);

// mprotect
error_t err = mm_user_utils_set_range_flags(
    vs, start, length_bytes,
    MM_USER_RANGE_FLAGS_DELTA, set_mask, clear_mask);
```

### 4. mprotect 实现

```c
error_t sys_mprotect(vaddr addr, size_t len, int prot) {
    // 1. 转换prot为ENTRY_FLAGS_t
    ENTRY_FLAGS_t set_mask = prot_to_flags(prot);
    ENTRY_FLAGS_t clear_mask = PAGE_ENTRY_WRITE;  // 清除写权限
    
    // 2. 使用编排层API
    error_t err = mm_user_utils_set_range_flags(
        vs, addr, len,
        MM_USER_RANGE_FLAGS_DELTA, set_mask, clear_mask);
    
    return err;
}
```

### 5. fork 地址空间复制

```c
// 简化版：使用core提供的API
error_t linux_copy_vspace(VSpace* parent, VSpace* child) {
    // 1. 遍历父进程的radix tree
    vaddr search_start = 0, search_end = USER_VADDR_MAX;
    while (search_start < search_end) {
        vaddr interval_start, interval_end;
        ENTRY_FLAGS_t flags;
        
        // 查找下一个已占用区间
        if (!vmm_radix_tree_find_first_occupied_interval(
                parent, search_start, search_end,
                &interval_start, &interval_end, &flags)) {
            break;
        }
        
        // 2. 复制到子进程（COW或直接复制）
        // ...
        
        search_start = interval_end;
    }
    return REND_SUCCESS;
}
```

### 6. 禁止的操作

```c
// ❌ 错误：直接操作radix内部数据结构
extern Radix_entry_t* vs->root_radix;
Radix_entry_t* l0 = &((Radix_entry_t*)vs->root_radix)[L0_INDEX(va)];
// 不要这样做！使用公开API

// ❌ 错误：在持PMM zone锁时调用map
pmm_zone_lock(pmm->zone);
map(vs, ppn, vpn, 3, flags, handler);  // 死锁风险！
pmm_zone_unlock(pmm->zone);

// ❌ 错误：忘记检查range_end
// 必须先调用vmm_radix_tree_calculate_end_check

// ❌ 错误：混淆kind参数
// INSERT用于预留区间，DELETE用于删除，QUERY_OR_CHANGE用于修改
```

## 📋 常见场景

### mmap匿名页
```c
vaddr addr = mm_user_utils_set_range_and_fill(vs, va, page_count, flags);
```

### munmap
```c
// 先查询ppn（可通过rmap或其他方式记录）
error_t err = mm_user_utils_clean_range_and_unfill(
    vs, va, page_count, ppn_first);
```

### page fault（lazy allocation）
```c
error_t err = mm_user_utils_fill_page_with_exist_range(vs, fault_va, flags);
```

### fork COW
```c
// 使用core提供的复制API，或遍历父进程radix tree
// 对每个区间设置COW标志（共享读，写时分裂）
```

### mprotect
```c
error_t err = mm_user_utils_set_range_flags(
    vs, addr, len,
    MM_USER_RANGE_FLAGS_DELTA, set_mask, clear_mask);
```

## 🔍 调试技巧

### 检查锁序
```c
// 确保锁序正确
// L0 big lock → L2 band lock → PMM zone lock → vspace_lock
```

### 验证区间状态
```c
ENTRY_FLAGS_t flags;
tagged_ptr_t owner;
error_t err = vmm_radix_tree_query_range(
    vs, va, va + PAGE_SIZE, &flags, &owner);
// 检查flags是否包含LAZY/VALID
```

### 查找已占用区间
```c
vaddr start, end;
ENTRY_FLAGS_t flags;
bool found = vmm_radix_tree_find_first_occupied_interval(
    vs, search_start, search_end, &start, &end, &flags);
```

## 📚 参考资料

- **core/include/rendezvos/mm/vmm_radix_tree.h** - Radix Tree API详细文档
- **core/include/rendezvos/mm/mm_user_utils.h** - 编排层API文档
- **core/docs/memory.md** - 内存系统设计文档（已更新）
- **doc/linux_compat/MM_AND_COW.md** - Linux兼容层内存设计
- **doc/ai/MM_VSPACE_RADIX_LAYERING.md** - 分层架构详解

---

**最后更新**：Radix Tree重构完成后  
**状态**：✅ 当前架构  
**优先级**：🔥 P0 - 影响正确性
