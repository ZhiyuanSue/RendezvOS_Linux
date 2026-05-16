# Nexus函数层级对照表

## 层级定义

基于cursor的L0-L3分层：

- **L0**: nexus自举层（Entry Pool / Manager Pages）- nexus自身的元数据管理
- **L1**: nexus后端数据结构层（Trees/Indices）- 纯粹的区间树CRUD
- **L2**: 页表/物理页后端适配层（PT + PMM + rmap）- 封装页表和物理页操作
- **L3**: nexus前端原语层（Transaction / Range Ops）- 给Linux层的稳定API
- **L4**: 上层策略（linux_layer）- Linux语义和策略

---

## 函数分类表

### L0: nexus自举层（Entry Pool / Manager Pages）

| 函数 | 行数 | 当前职责 | 问题描述 |
|------|------|----------|----------|
| `is_page_manage_node` | 252-255 | 判断是否为管理页节点 | ✅ 符合L0 |
| `nexus_init_manage_page` | 256-274 | 初始化管理页，建立空闲链表 | ✅ 符合L0 |
| `nexus_get_free_entry` | 356-432 | 从节点池分配一个nexus_node | ⚠️ 混合了L0（分配）和L1（插入树） |
| `nexus_free_entry` | 459-484 | 释放nexus_node回池 | ✅ 符合L0 |
| `free_manage_node_with_page` | 433-458 | 释放整个管理页 | ⚠️ 混合了L0和L2（unmap/pmm_free） |
| `init_vspace_nexus` | 275-324 | 初始化vspace的nexus根节点 | ⚠️ 混合了L0（管理页）、L1（树操作）、L3（锁初始化） |

**L0问题总结**:
- `nexus_get_free_entry` 在分配节点后还涉及后续操作，应该只负责分配
- `free_manage_node_with_page` 直接调用unmap和pmm_free，越过了L2
- `init_vspace_nexus` 职责过多，应该拆分

---

### L1: nexus后端数据结构层（Trees/Indices）

| 函数 | 行数 | 当前职责 | 问题描述 |
|------|------|----------|----------|
| `nexus_rb_tree_insert` | 146-165 | 向vspace树插入节点 | ✅ 符合L1 |
| `nexus_rb_tree_vspace_insert` | 166-185 | 向vspace注册树插入 | ✅ 符合L1 |
| `nexus_rb_tree_remove` | 186-192 | 从vspace树删除节点 | ✅ 符合L1 |
| `nexus_rb_tree_vspace_remove` | 193-201 | 从vspace注册树删除 | ✅ 符合L1 |
| `nexus_rb_tree_vspace_search` | 202-218 | 在vspace注册树查找 | ✅ 符合L1 |
| `nexus_rb_tree_search` | 219-235 | 在vspace树查找覆盖地址的节点 | ✅ 符合L1 |
| `nexus_rb_tree_prev` | 236-243 | 获取前驱节点 | ✅ 符合L1 |
| `nexus_rb_tree_next` | 244-251 | 获取后继节点 | ✅ 符合L1 |
| `insert_nexus_entry` | 703-720 | 插入nexus节点（树+链表） | ✅ 符合L1 |
| `delete_nexus_entry` | 721-732 | 删除nexus节点（树+链表+池） | ⚠️ 调用了nexus_free_entry（L0） |
| `_take_range` | 764-841 | 分配虚拟地址范围（只创建节点） | ⚠️ 混合了L0（分配节点）和L1（插入树） |

**L1问题总结**:
- 红黑树操作本身很纯粹，符合L1
- `delete_nexus_entry` 跨层调用L0，应该通过接口
- `_take_range` 混合了节点分配和树插入，应该拆分

---

### L2: 页表/物理页后端适配层（PT + PMM + rmap）

| 函数 | 行数 | 当前职责 | 问题描述 |
|------|------|----------|----------|
| `link_rmap_list` | 26-64 | 链接nexus节点到物理页rmap | ✅ 符合L2 |
| `unlink_rmap_list` | 65-74 | 从物理页rmap删除节点 | ✅ 符合L2 |
| `nexus_update_node` | 92-145 | 更新单个叶子映射（含COW分裂） | ⚠️ 混合了L2（map/unmap/rmap）和L3（事务/回滚） |
| `nexus_map_anon_zero_leaf` | 1832-1906 | 分配并映射零页 | ⚠️ 混合了L2（map）和L0（pmm_alloc） |
| `unfill_phy_page` | 2200-2270 | 通过物理页反向解映射 | ✅ 符合L2（主要是rmap操作） |
| `nexus_kernel_page_owner_cpu` | 2272-2317 | 查询内核页的拥有者CPU | ✅ 符合L2（rmap查询） |

**L2问题总结**:
- `nexus_update_node` 职责过重，包含了COW分裂的完整事务逻辑
- `nexus_map_anon_zero_leaf` 直接调用pmm_alloc，应该通过L2接口

---

### L3: nexus前端原语层（Transaction / Range Ops）

| 函数 | 行数 | 当前职责 | 问题描述 |
|------|------|----------|----------|
| `nexus_reserve_user_range` | 1753-1786 | 预留用户虚拟地址范围 | ✅ 符合L3 |
| `nexus_commit_user_range` | 1788-1802 | 提交预留范围（分配物理页） | ✅ 符合L3 |
| `nexus_uncommit_user_range` | 1804-1816 | 取消提交（释放物理页） | ✅ 符合L3 |
| `nexus_release_user_range` | 1818-1830 | 释放预留范围（删除nexus） | ✅ 符合L3 |
| `nexus_update_range_flags` | 1984-2087 | 更新范围标志（批量+回滚） | ✅ 符合L3（但内部有跨层调用） |
| `nexus_remap_user_leaf` | 2089-2165 | 重新映射单页（COW用） | ✅ 符合L3 |
| `nexus_query_vaddr` | 2167-2198 | 查询虚拟地址的nexus信息 | ✅ 符合L3 |
| `nexus_query_user_semantics` | 1711-1751 | 查询完整语义（nexus+PTE） | ✅ 符合L3 |
| `vspace_clone` | 1093-1247 | 克隆vspace（COPY或COW） | ✅ 符合L3（但函数过长） |
| `_vspace_clone_copy_page` | 843-871 | 克隆时复制单页 | ⚠️ 混合了L0（pmm_alloc）和L2（map） |
| `_vspace_clone_cow` | 873-902 | 克隆时设置COW | ⚠️ 混合了L2（map）和PMM（refcount） |
| `get_free_page` | 1908-1948 | 统一分配接口 | ✅ 符合L3 |
| `free_pages` | 1960-1982 | 统一释放接口 | ✅ 符合L3 |
| `nexus_create_vspace_root_node` | 485-549 | 创建vspace的nexus结构 | ⚠️ 混合了多层职责 |
| `nexus_delete_vspace` | 605-702 | 删除vspace的所有nexus节点 | ⚠️ 混合了多层职责 |
| `nexus_migrate_vspace` | 551-603 | 迁移vspace到其他CPU | ✅ 符合L3 |
| `cleanup_aux_list` | 747-763 | 清理临时借用的aux_list | ✅ 符合L3（辅助函数） |
| `nexus_update_flags_list_core` | 937-1026 | 批量更新标志核心实现 | ✅ 符合L3（但过长） |
| `_vspace_update_user_leaf_flags` | 1039-1091 | 更新vspace所有4K页标志 | ✅ 符合L3 |
| `nexus_range_compute_flags` | 904-919 | 计算新的页表标志 | ✅ 符合L3（辅助函数） |

**L3问题总结**:
- 大部分函数职责清晰，符合L3定位
- 主要问题是函数内部直接跨层调用（如直接pmm_alloc）
- 一些复杂函数过长，需要拆分

---

### 辅助/内部函数（难以简单分层）

| 函数 | 行数 | 当前职责 | 问题描述 |
|------|------|----------|----------|
| `nexus_node_get_len` | 75-80 | 获取节点长度 | ✅ 纯函数，可在任何层 |
| `nexus_node_get_pages` | 81-84 | 获取节点页数 | ✅ 纯函数 |
| `nexus_node_set_len` | 85-90 | 设置节点为2M或4K | ✅ 纯函数 |
| `nexus_node_vspace` | 66-78 | 从节点获取vspace | ✅ 纯函数 |
| `nexus_root_heap_ref` | 79-85 | 获取kernel heap ref | ✅ 纯函数 |
| `nexus_get_vspace_node` | 1699-1709 | 从VS_Common获取vspace节点 | ✅ 纯函数 |
| `init_nexus` | 326-355 | 初始化per-CPU nexus根 | ⚠️ 混合了多层 |
| `_user_take_range` | 1484-1508 | 用户空间的reserve操作 | ⚠️ 只是简单包装 |
| `user_fill_range` | 1395-1483 | 为用户范围分配物理页 | ⚠️ 混合了L0/L2/L3 |
| `user_unfill_range` | 1633-1650 | 解映射用户范围 | ⚠️ 混合了多层 |
| `_unfill_range` | 1509-1559 | 解映射的内部实现 | ⚠️ 混合了L2/L3 |
| `_kernel_get_free_page` | 1249-1390 | 内核分配虚拟内存 | ⚠️ 混合了所有层 |
| `_kernel_free_pages` | 1560-1627 | 释放内核虚拟内存 | ⚠️ 混合了多层 |
| `_user_release_range` | 1652-1697 | 释放用户范围的nexus节点 | ⚠️ 只是简单包装 |

---

## 架构Violations统计

### 跨层调用问题

1. **L3直接调用L0**: `nexus_update_range_flags` 等函数内部直接操作节点池
2. **L3直接调用PMM**: `get_free_page`, `_vspace_clone_copy_page` 等直接pmm_alloc
3. **L2混合L3逻辑**: `nexus_update_node` 包含了完整的事务和回滚逻辑
4. **L1混合L0**: `_take_range` 混合了节点分配和树插入

### 职责过重的函数

1. **`vspace_clone` (155行)**: 混合了基础设施创建、映射复制、COW设置
2. **`nexus_update_range_flags` (104行)**: 混合了验证、收集、更新
3. **`_kernel_get_free_page` (142行)**: 混合了PMM分配、nexus创建、页表映射
4. **`nexus_update_flags_list_core` (90行)**: 批量更新和完整回滚
5. **`nexus_delete_vspace` (98行)**: 清理所有映射和节点
6. **`user_fill_range` (89行)**: 映射和回滚
7. **`nexus_get_free_entry` (77行)**: 分配器逻辑

---

## 重构优先级建议

### 第一优先级：建立清晰的层间接口

**目标**: 消除直接跨层调用，建立接口函数

1. **L0→L1接口**: `nexus_alloc_node()`, `nexus_free_node()`
2. **L1→L2接口**: `nexus_pt_map_leaf()`, `nexus_pt_unmap_leaf()`, `nexus_pt_remap_leaf()`
3. **L2→PMM接口**: `nexus_pmm_alloc()`, `nexus_pmm_free()`, `nexus_pmm_ref()`

### 第二优先级：拆分复杂函数

**目标**: 每个函数<100行，单一职责

1. `vspace_clone`: 拆分为基础设施、复制、COW三个函数
2. `nexus_update_range_flags`: 拆分为收集、更新两个阶段
3. `_kernel_get_free_page`: 拆分为分配、创建、映射三个步骤
4. `nexus_update_flags_list_core`: 拆分为更新和回滚

### 第三优先级：分离纯粹的数据结构操作

**目标**: L1只负责区间树，不碰页表和物理页

1. 提取红黑树操作到独立文件
2. `_take_range` 拆分为节点分配（L0）和树插入（L1）
3. `delete_nexus_entry` 拆分为树删除（L1）和节点释放（L0）

---

## 三个后端的澄清

### Backend-A: Nexus区间/元数据后端（L0 + L1）
- **职责**: 管理虚拟地址区间的元数据
- **数据结构**: 红黑树 + 节点池
- **操作**: 插入/删除/查找/分裂/合并区间

### Backend-B: 页表后端（L2的一部分）
- **职责**: 封装页表操作
- **接口**: map_handler (map/unmap/have_mapped)
- **操作**: 映射/解映射/查询页表

### Backend-C: 物理页后端（L2的另一部分）
- **职责**: 管理物理页分配和引用计数
- **接口**: PMM (pmm_alloc/pmm_free/pmm_change_pages_ref)
- **操作**: 分配/释放/引用计数

**当前问题**: 这三个后端的操作混在一起，没有清晰的接口边界。
