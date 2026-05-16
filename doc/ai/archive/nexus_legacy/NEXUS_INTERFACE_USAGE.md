# Nexus接口使用分析

## 概述

分析nexus接口在core/和linux_layer中的使用情况，识别哪些是**核心稳定接口**（不能轻易改动），哪些是**内部接口**（可以重构）。

---

## Core/中的使用

### 1. 内核内存分配（kmalloc.c）

**使用的接口**：
- `get_free_page()` - 分配内核虚拟内存
- `free_pages()` - 释放内核虚拟内存
- `nexus_kernel_page_owner_cpu()` - 查询内核页的拥有者CPU
- `kinit(struct nexus_node* nexus_root)` - 初始化，接受nexus_root参数

**特点**：
- 只使用**内核路径**（target_vaddr >= KERNEL_VIRT_OFFSET）
- 依赖旧的统一接口（get_free_page/free_pages）
- `nexus_kernel_page_owner_cpu`用于kmem路由（跨CPU释放）

**稳定性要求**：⭐⭐⭐⭐
- kmalloc是core/基础设施
- 不应该轻易改接口
- 但可以内部重构实现

---

### 2. 虚拟地址空间管理（vmm.c）

**使用的接口**：
- `init_nexus()` - 初始化per-CPU nexus根
- `nexus_delete_vspace()` - 删除vspace的nexus视图

**特点**：
- 只在初始化和清理时使用
- 不涉及日常分配路径

**稳定性要求**：⭐⭐⭐
- `init_nexus`是启动关键路径
- `nexus_delete_vspace`是清理关键路径
- 但调用频率低，重构空间大

---

### 3. ELF加载和线程创建（thread_loader.c）

**使用的接口**：
- `get_free_page()` - 为用户栈/ELF分配内存
- `nexus_create_vspace_root_node()` - 为新vspace创建nexus根节点

**特点**：
- 用户路径（target_vaddr < KERNEL_VIRT_OFFSET）
- `nexus_create_vspace_root_node`只在进程创建时调用

**稳定性要求**：⭐⭐⭐
- ELF加载是启动关键路径
- 但调用频率低，重构空间中等

---

### 4. SMP启动（arch/*/boot/smp.c）

**使用的接口**：
- `get_free_page()` - 为AP栈分配内核内存

**特点**：
- 只在启动时调用
- 内核路径

**稳定性要求**：⭐⭐
- 启动时使用，但可以重构

---

### 5. 测试代码（modules/test/）

**使用的接口**：
- `nexus_update_range_flags()` - 测试标志更新
- 直接访问nexus内部结构（`nexus_root->manage_free_list`）

**特点**：
- 测试代码可以随意改
- 但要注意测试的是公共接口还是内部实现

**稳定性要求**：⭐
- 测试代码可以随实现修改

---

## Linux Layer中的使用

### 1. 页故障处理（linux_page_fault_irq.c）

**使用的接口**：
- `nexus_query_vaddr()` - 查询虚拟地址的nexus信息
- `nexus_remap_user_leaf()` - COW分裂时重新映射

**特点**：
- **热路径**：每次页故障都会调用
- 依赖nexus作为"语义真源"
- `nexus_remap_user_leaf`封装了COW分裂的复杂逻辑

**稳定性要求**：⭐⭐⭐⭐⭐
- 这是COW的核心
- 接口必须保持稳定
- 但内部实现可以重构

---

### 2. 内存保护（sys_mprotect.c）

**使用的接口**：
- `nexus_update_range_flags()` - 更新范围的访问权限

**特点**：
- 系统调用路径
- 支持mprotect/mremap等

**稳定性要求**：⭐⭐⭐⭐⭐
- Linux兼容层核心功能
- 接口必须稳定

---

## 接口分类总结

### A. 核心稳定接口（不能轻易改动）

| 接口 | 使用者 | 原因 |
|------|--------|------|
| `get_free_page()` | kmalloc, ELF, SMP | 内核内存分配主接口 |
| `free_pages()` | kmalloc | 内核内存释放主接口 |
| `nexus_remap_user_leaf()` | linux_layer | COW分裂核心 |
| `nexus_query_vaddr()` | linux_layer | 页故障查询 |
| `nexus_update_range_flags()` | linux_layer, 测试 | mprotect核心 |
| `nexus_create_vspace_root_node()` | thread_loader | 进程创建 |
| `nexus_delete_vspace()` | vmm | 进程销毁 |
| `init_nexus()` | vmm, SMP启动 | 系统初始化 |

**策略**：
- 保持接口签名不变
- 可以内部重构实现
- 如果必须改接口，需要同步更新所有调用者

---

### B. 内部接口（可以重构）

| 接口 | 当前使用者 | 可以重构为 |
|------|-----------|-----------|
| `nexus_reserve_user_range()` | 无（计划中） | L3接口 |
| `nexus_commit_user_range()` | 无（计划中） | L3接口 |
| `nexus_uncommit_user_range()` | 无（计划中） | L3接口 |
| `nexus_release_user_range()` | 无（计划中） | L3接口 |
| `nexus_map_anon_zero_leaf()` | 无（计划中） | L3/L2接口 |
| `nexus_query_user_semantics()` | 无（计划中） | L3接口 |
| `nexus_kernel_page_owner_cpu()` | kmalloc | L2查询接口 |

**策略**：
- 这些是"计划中的新接口"
- 可以自由设计
- 用于替换旧的`get_free_page/free_pages`

---

### C. 红黑树和内部函数（完全内部）

| 接口 | 使用者 | 策略 |
|------|--------|------|
| `nexus_rb_tree_*` | nexus.c内部 | 完全内部，可随意重构 |
| `link_rmap_list/unlink_rmap_list` | nexus.c内部 | L2接口，可重构 |
| `nexus_get_free_entry/free_entry` | nexus.c内部 | L0接口，可重构 |
| `_take_range` | nexus.c内部 | L1接口，可重构 |

**策略**：
- 这些是nexus内部实现
- 重构时不需要考虑外部影响
- 但要小心锁顺序和并发

---

## 重构策略建议

### 第一阶段：建立新的L3接口

**目标**：引入新接口，不破坏现有使用

1. **实现新的L3接口**：
   ```c
   error_t nexus_reserve_user_range(...);
   error_t nexus_commit_user_range(...);
   error_t nexus_uncommit_user_range(...);
   error_t nexus_release_user_range(...);
   ```

2. **保持旧接口兼容**：
   ```c
   // 旧的get_free_page/free_pages内部调用新接口
   void* get_free_page(...) {
       // 内部实现改为调用nexus_reserve/commit
   }
   ```

3. **逐步迁移使用者**：
   - 先迁移linux_layer（sys_mmap等）
   - 再迁移core/（kmalloc等）

---

### 第二阶段：重构内部实现

**目标**：拆分文件，建立清晰的L0/L1/L2

1. **提取L0**（nexus_pool.c）：
   - `nexus_alloc_node/free_node`
   - `nexus_init_manage_page`
   - 不影响外部接口

2. **提取L1**（nexus_range.c）：
   - 红黑树操作
   - `nexus_range_insert/remove/find`
   - 不影响外部接口

3. **提取L2**（nexus_pt.c）：
   - `nexus_pt_alloc_and_map/unmap_and_free`
   - `link_rmap_list/unlink_rmap_list`
   - 不影响外部接口

---

### 第三阶段：引入backend_ops

**目标**：支持可插拔后端（anon/file）

1. **定义backend_ops接口**：
   ```c
   struct nexus_backend_ops {
       error_t (*range_bind)(...);
       error_t (*leaf_populate)(...);
       error_t (*leaf_write_fault)(...);
       error_t (*leaf_evict_prepare)(...);
   };
   ```

2. **扩展nexus_node**：
   ```c
   struct nexus_node {
       // ...
       enum nexus_backend_type backend_type;
       void* backend_private;
       const struct nexus_backend_ops* ops;
   };
   ```

3. **重构L3调用backend_ops**：
   - `nexus_commit_user_range`调用`ops->leaf_populate`
   - `nexus_remap_user_leaf`调用`ops->leaf_write_fault`

---

## 关键约束

### 1. 不能破坏的接口

**绝对不能改**：
- `get_free_page/free_pages`的语义（内核/用户分配）
- `nexus_remap_user_leaf`的COW分裂语义
- `nexus_update_range_flags`的原子更新语义

**可以内部重构**：
- 实现方式
- 内部数据结构
- 锁策略

---

### 2. 性能约束

**热路径**：
- 页故障处理（nexus_query_vaddr + nexus_remap_user_leaf）
- 内核内存分配（get_free_page内核路径）

**不能引入的性能退化**：
- 额外的函数调用层
- 更复杂的锁竞争
- 更多的内存分配

---

### 3. 并发约束

**锁顺序**：
1. vspace->nexus_vspace_lock
2. pmm_zone_lock

**永远不要反向获取**，否则死锁！

---

## 测试策略

### 回归测试

**必须保持通过的测试**：
- kmalloc测试（单核和SMP）
- nexus测试（单核和SMP）
- 页故障测试（COW、lazy allocation）
- Linux兼容层测试（mmap, mprotect, fork）

### 新增测试

**backend_ops测试**：
- ANON后端的基本功能
- 后端切换的边界情况
- 错误处理和回滚

---

## 总结

**核心发现**：
1. core/主要使用旧的统一接口（get_free_page/free_pages）
2. linux_layer使用新的细粒度接口（nexus_remap_user_leaf等）
3. 有8个核心接口必须保持稳定
4. 有大量内部接口可以自由重构

**重构优先级**：
1. 先建立新的L3接口（不破坏现有代码）
2. 再重构内部实现（拆分文件，建立L0/L1/L2）
3. 最后引入backend_ops（支持file后端）

**风险控制**：
- 每一步都要保持所有测试通过
- 热路径要特别注意性能
- 锁顺序必须严格遵守
