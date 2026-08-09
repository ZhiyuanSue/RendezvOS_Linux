# VFS 动态存储（page_slice 表）

> **Status**: S0–S3 已落地（2026-07-27）  
> **从属**: 原 busybox 容量妥协，见 [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md)（已归档；容量项已回收）  
> **不阻塞**: busybox Path B / execve / `run_all.sh` 回收（那些仍是更高优先级）

## 问题

`servers/fs/` 里大量 **BSS 定长表**（cpio/ns 曾抬到 2048）和 **栈上大数组**（`ramfs_readdir` 的 `names[128][64]`、`vfs_open` 的 `chunk[4096]`）是演示期妥协，会随 rootfs 增长静默 OOM/栈溢出。

## 原则

`page_slice` = **可增长的逻辑连续字节空间**（按 pgoff 挂页）。

约束与 corner case：

1. **扩容不搬迁旧页** → 已映射页的 KVA 在 grow 后仍有效（namespace 指针 / mount view 指针 / pcache LRU 节点可保留）。  
2. **记录不得跨页**：`elem_size ≤ PAGE_SIZE`，每页 `PAGE_SIZE / elem_size` 个完整记录。  
3. **软上限** `VFS_SLICE_TABLE_SOFT_MAX`（默认 1<<20），小表可在 init 后下调 `soft_max`。  
4. **OOM**：grow / `m_alloc` 失败返回错误；`ensure_cap` 中途失败回滚逻辑 size。  
5. **VFS listen 单线程**：表无内部锁。  
6. **readdir**：临时 slice 表存名字；插入失败传播 `-ENOMEM`。  
7. **ramfs 条目**：原地 tombstone（`alive`），禁止 unlink 时 swap-with-last。  
8. **push 后 ptr 失败**：一律 `pop_last`，避免 orphan 槽。

## S0–S3 状态与方案

| 级 | 形态 | 状态 | 做法 |
|----|------|------|------|
| **S0** | 大块 BSS 定长表 | ✅ | cpio / ns / ramfs / handle / blkdev / fd |
| **S1** | 栈上大数组 | ✅ | I/O scratch + readdir names；残留 `char[256]` OK |
| **S2** | 每项嵌 `path[256]` | ✅ ns | **仅** `vfs_ns_node`：已有 `parent`+`name`，按需 `vfs_ns_path_of` |
| **S2** | 扁平表 path | ⬜ 保留 | cpio/ramfs：**path 是主键**，非 parent+name 冗余 |
| **S2** | 句柄/fd path | ⬜ 保留 | `vfs_inode` / fd 表：打开后路径快照（IPC / openat） |
| **S3** | 小固定槽 | ✅ | mount / backend registry / pcache → `vfs_slice_table` |

### 为何不砍扁平后端的 path？

cpio catalog 与 ramfs entry 是 **path → 记录** 的平铺索引，没有树链接；去掉 path 等于换一套索引（hash / inode 号），超出「去冗余」范围。

### 为何保留 inode/fd path？

- open handle 需稳定路径做 getdents / busy / pcache key  
- compat fd 表需 abs path 做 openat/chdir  
这两处不是「随 catalog 条数线性膨胀的表字段」主矛盾。

## 已落地

| 组件 | 做法 |
|------|------|
| `vfs_slice_table` | push/pop/`vfs_dir_names_insert`; ensure_cap rollback; pop_last on ptr failure |
| cpio / ramfs / handle / ns 表 | growable |
| ramfs unlink | tombstone（指针稳定） |
| namespace 节点 | **无** `path[]`；`vfs_ns_path_of` 重建 |
| mount / backend / pcache 槽 | growable（软上限 64 / 32 / 64） |
| vfs_open I/O / blkdev | heap / page_slice |
| pcache eviction | dirty flush 失败不丢数据 |

## 建议后续（扩展性；不阻塞 busybox）

1. readdir 按 index 扫描，避免每次重建全名字表（大目录 O(n²)）  
2. tombstone / deleted 槽压缩或世代号回收  
3. 下调 cpio/ns/ramfs/handle 的默认 `soft_max` 到更贴近 boot 的上限  
4. ramfs 文件体 → page_slice（对齐 page cache；抬高 256KiB 策略上限）  
5. 打开句柄改持 `vfs_ns_node *` + 世代号（再谈砍 inode.path）
