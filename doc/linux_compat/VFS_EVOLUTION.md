# VFS 演进路线图（待办）

> **基线（2026-07-12）**：树形 `vfs_namespace` + cpio page cache + mount 登记/覆盖 + bootstrap 权限 + renameat/linkat。  
> **验证**：x86_64 **52/52**；aarch64 **52/52**。

---

## 状态图例

| 标记 | 含义 |
|------|------|
| ✅ | 已实现 |
| 🚧 | 骨架 / 部分 |
| ⬜ | 待做 |

---

## 架构分层（当前）

```
linux_layer/fs/          compat：fd 表、cwd、path 展开、syscall → IPC
        ↕ IPC (vfs_protocol.h)
servers/fs/              中端：namespace 树、page cache、mount、perm、ramfs/cpio
```

| 模块 | 文件 | 职责 |
|------|------|------|
| 命名空间 | `vfs_namespace.c` | 树形 dentry、lookup/mkdir/unlink/rename/link、readdir |
| 后端 | `ramfs_layer.c`, `cpio_rofs` | 字节存储；路径状态在 namespace |
| 缓存 | `vfs_page_cache.c` | cpio 小文件读缓存（≤256KiB） |
| 挂载 | `vfs_mount.c` | mount 登记、覆盖 cpio 子项、umount EBUSY/DETACH |
| 权限 | `vfs_perm.c` | request cred + owner mode 位 |
| 路径 | **`include/linux_compat/fs/vfs_path.h` + `linux_layer/fs/vfs_path.c`** | 唯一实现；compat 仅 `linux_vfs_resolve_path` |
| RPC 面 | `vfs_open.c`, `vfs_server.c` | open/handle I/O + dispatch |
| Facade | `vfs_root.c` | init + 薄封装 |

---

## 重构合并清单（§Merge）

演进过程中 **应合并** 的重复逻辑：

| ID | 重复项 | 状态 | 目标 |
|----|--------|------|------|
| M1 | `servers/fs/vfs_path.c` ↔ `linux_vfs_path.c` normalize | ✅ | 单模块 `linux_layer/fs/vfs_path.c` + `linux_compat/fs/vfs_path.h` |
| M2 | `VFS_PATH_MAX` ↔ `LINUX_VFS_PATH_MAX` | 🚧 | 均为 256；可 `#define LINUX_VFS_PATH_MAX VFS_PATH_MAX` |
| M3 | `vfs_kern_load.c` ↔ `vfs_exec_load.c` 灌 slice | 🚧 | 已共用 `linux_page_slice_copy_from_kva`；可再抽 `vfs_read_into_slice()` |
| M4 | cpio read：`vfs_page_cache` vs `vfs_backend` direct | ⬜ | 统一经 page cache 或明确分层边界 |
| M5 | `vfs_perm` request cred vs 未来 `setuid` compat | ⬜ | cred 真源只在 `linux_proc_append_t` |
| M6 | 架构文档 vs `VFS_EVOLUTION` vs `VFS_IMPLEMENTATION_STATUS` | 🚧 | 以本文 + ARCHITECTURE 为准，IMPLEMENTATION_STATUS 做 live 索引 |

**不必合并**（分层正确）：

- compat **fd 表** vs server **handle 表**（方案 B）
- compat **路径展开** vs server **namespace lookup**
- **page_slice 加载**（内核 ingest）vs 用户态 **read/write IPC**

---

## Path 规范化（§Path）

**栈策略**（非“零栈”，而是 **小栈 + 不复制路径体**）：

1. collapse 只存 `(offset, len)` 指向 scratch 内分量，~100B 元数据。
2. `normalize` 一层 `scratch[256]`，结果写入 **调用者的 `out`**。
3. 无 `join` 递归。

**后续（若要求工作区完全离栈）**：caller-scratch API 或 kallocator 单次 scratch（见 P5-5）。

---

## fd 表与 page_slice（§FdSlice）

### 当初设计意图（未完全落地）

- **core `page_slice`**：内核侧逻辑连续字节流（buddy ≤2MiB/块），pgoff→kva 索引。
- **设想**：打开的文件 / mmap 后端用 slice 表示内容；**fork 时**对 slice 做 **`page_slice_copy_to_slice`**（core 已有 API），子进程独立副本或 COW 策略。
- **fd 表**：fd → `{ kind, slice? | handle, path }`。

### 当前实现（方案 B，2026-07）

| 能力 | 实现 |
|------|------|
| 用户 open/read/write | compat fd → **VFS server handle** → cpio/ramfs/page_cache |
| fd 条目 | `linux_fd_entry_t`：`vfs_handle` + `vfs_abs_path`，**无 page_slice** |
| fork | `linux_fs_proc_fork`：`memcpy` fs 状态 + 每个唯一 handle **IPC RETAIN** + pipe retain |
| execve | `linux_fs_proc_reset`：release handles，重置 0/1/2 |
| 内核读 ELF/manifest | **page_slice**（`vfs_kern_read_file_slice` / `linux_vfs_read_file_for_exec_slice`）→ `load_elf_to_vs` → **destroy** |
| core slice 复制 | `page_slice_copy_to_slice` / `copy_to_buffer` / `copy_to_user` **存在**，compat **未用于 fd/fork** |

### 缺口（待设计）

| ID | 项 | 说明 |
|----|-----|------|
| F1 | fd 绑定 slice | 未做；文件内容在 server inode，不在进程 slice |
| F2 | fork copy slice | 未做；fork 只共享 server handle（同 inode 偏移） |
| F3 | mmap 文件后端 | Phase 2+；radix + 可能 slice 或按需 fault |
| F4 | `page_slice_clone` 封装 | 可在 compat 包一层 `linux_page_slice_dup()` 调 core `copy_to_slice` |

**与测例关系**：当前 52/52 不依赖 F1–F2；execve/harness 的 slice 生命周期是 **load→map→destroy**，与 fd 表分离（见 [`FILE_LOADING.md`](FILE_LOADING.md) §5）。

---

## P0 — 测例阻塞

| ID | 项 | 状态 |
|----|-----|------|
| P0-1 | `openat(dirfd, rel, O_CREATE)` | ✅ |
| P0-2 | 进程 exit 释放 VFS handle | ✅ |
| P0-3 | rootfs 布局澄清 | ✅ |
| P0-4 | aarch64 path 栈溢出（execve） | ✅ |

---

## P1 — 命名空间结构

| ID | 项 | 状态 |
|----|-----|------|
| P1-1 | 树形 dentry | ✅ |
| P1-2 | readdir 持久子节点索引 | ✅ |
| P1-3 | 中间目录 materialize | ✅ |
| P1-4 | `..` / `.` 语义 | ✅ |

---

## P2 — 缓存与 I/O

| ID | 项 | 状态 |
|----|-----|------|
| P2-1 | page cache（cpio 小文件读） | ✅ |
| P2-2 | 写路径 cache 失效 | ✅ |
| P2-3 | dirty 页 / 写合并 | ⬜ |
| P2-4 | `O_DIRECT` | ⬜ |

---

## P3 — 挂载与多后端

| ID | 项 | 状态 |
|----|-----|------|
| P3-1 | mount 点登记 | ✅ |
| P3-2 | mount 覆盖 lookup | ✅ |
| P3-3 | `umount2` MNT_DETACH / EBUSY | ✅ |
| P3-4 | bind mount | ⬜ |
| P3-5 | mount 后 walk 真切换后端 | ⬜ |

---

## P4 — 权限与安全

| ID | 项 | 状态 |
|----|-----|------|
| P4-1 | mode bootstrap（root 绕过） | ✅ |
| P4-2 | uid/gid / euid（owner 位） | ✅ |
| P4-3 | group/other、sticky | ⬜ |
| P4-4 | `O_NOFOLLOW` / symlink | ⬜ |

---

## P5 — 兼容层 / RPC

| ID | 项 | 状态 |
|----|-----|------|
| P5-1 | `renameat` / `renameat2` | ✅ |
| P5-1b | `linkat`（ramfs） | ✅ |
| P5-2 | `statx` / `faccessat2` | ⬜ |
| P5-3 | OPEN bit31 `is_dir` | ✅ |
| P5-4 | `readlinkat` / `symlinkat` | ⬜ |
| P5-5 | path caller-scratch / kmalloc 工作区 | ⬜ |
| **P5-6** | **path 单模块合并（M1）** | **✅** |

---

## P6 — 文档

| ID | 项 | 状态 |
|----|-----|------|
| P6-1 | `VFS_ARCHITECTURE.md` 对齐 | 🚧 |
| P6-2 | `RAMFS_AND_VFS_STORAGE.md` | 🚧 |
| P6-3 | `ROOTFS.md` | ✅ |
| P6-4 | `VFS_IMPLEMENTATION_STATUS.md` 同步 | 🚧 |
| P6-5 | `FD_TABLE.md` §page_slice 缺口 | 🚧 |

---

## 完成度概览

| 阶段 | 进度 |
|------|------|
| P0 测例阻塞 | **100%** |
| P1 命名空间 | **100%** |
| M1 path 合并 | **100%** |
| P2 缓存 | **50%** |
| P3 挂载 | **60%** |
| P4 权限 | **50%** |
| P5 syscall/RPC | **60%** |
| F fd↔slice | **0%**（设计保留，方案 B 未走 slice） |
| P6 文档 | **45%** |

---

## 下一批建议

1. **F1/F2 设计评审**：fd 是否继续纯 handle，还是在 mmap/大文件时再引入 slice + fork dup。
2. **M3**：统一 kernel 侧 read→slice 入口。
3. **P3-5**：mount 真切换后端。
4. **P5-2**：statx RPC。

---

## 验证门

```bash
make ARCH=x86_64 user && make ARCH=x86_64 build && make ARCH=x86_64 run
make ARCH=aarch64 user && make ARCH=aarch64 build && make ARCH=aarch64 run
```

---

## 参考

- [`ROOTFS.md`](ROOTFS.md)
- [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md)
- [`FD_TABLE.md`](FD_TABLE.md)
- [`FILE_LOADING.md`](FILE_LOADING.md)
