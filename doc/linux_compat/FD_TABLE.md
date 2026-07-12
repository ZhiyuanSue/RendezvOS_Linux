# Per-process fd table（方案 B + page_slice 容器）

> **Status**: 定稿（2026-07-12）— compat 本地 fd 表；**表体**在 core `page_slice` 中  
> **前置**: [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) · **演进**: [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md) §FdSlice  
> **Live 索引**: [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)

---

## 1. 为什么要改

Bootstrap 阶段 fd 编号表在 `vfs_server`，不符合 Linux per-process fd 模型。**方案 B**：compat **本地 fd 表** + server **全局 open handle 表**。

2026-07 进一步把 fd 表从「单块 kmalloc 结构体」迁到 **page_slice**，以便动态扩容 fd 槽位，并与 core slice 基础设施对齐（fork 用 `page_slice_clone`）。

---

## 2. Linux 对照

| Linux | RendezvOS |
|-------|-----------|
| `struct file` | `vfs_open_handle_t`（server） |
| `files_struct->fd[]` | `linux_proc_append_t.fs` → **`page_slice` 表** |
| `dup` / `dup2` | compat 改 fd 槽；VFS **RETAIN** handle |
| `fork` | **`page_slice_clone`** fd 表 + 每唯一 handle **RETAIN** |
| `execve` / exit | release handles；exec **重建** 0/1/2；exit **不重建** slice（TASK_REAP fini 销毁） |

**Per-process**：fd 表在线程组（`Tcb_Base` / `linux_proc_append_t`）间共享，不按 tid 索引。

---

## 3. 分层与数据流

```text
linux_layer
  linux_proc_append_t.fs → linux_fs_state_t { page_slice *table }
  sys_openat: resolve abs path → IPC OPEN → linux_fd_alloc
  sys_read/write/close: linux_fd_get → CONSOLE / PIPE / VFS IPC
        ↕ IPC (handle + path)
servers/fs
  vfs_handle_t[id]: refcnt, inode, offset
  vfs_namespace: 路径 lookup（树形 dentry）
  **无** per-pid fd 编号表
```

---

## 4. page_slice 布局（定稿）

`linux_fs_state_t` 仅持 **`struct page_slice *table`**。逻辑字节布局：

```text
byte 0 .. ~259          linux_fs_slice_hdr_t { cwd[256], fd_capacity }
byte 260 .. 8191        保留（LINUX_FS_FD_REGION_BASE = 2×PAGE_SIZE）
byte 8192 + fd×ent_size linux_fd_entry_t[fd]
```

**`linux_fs_slice_hdr_t` 是什么？**  
仅 **slice 头部元数据**：当前工作目录字符串 + 当前 fd 槽容量。**不是** fd 条目本身；每个 fd 的 `kind/handle/path` 在 **8KiB 之后** 按索引存放。

**为何不在栈上放 hdr？**  
旧实现错误地在 syscall 路径上用 `linux_fs_slice_hdr_t hdr` 作局部变量。迁移前 hdr 含 16 路 `dir_paths[]`，体积 **4372B**；内核 kstack 仅 **8KiB**，嵌套调用会栈溢出并破坏无关内存（曾在 `del_vspace` 表现为 radix GP）。  
**正确做法**：hdr 只存在于 **page_slice 持久存储**；内核侧读写通过 **per-CPU scratch** 或 **4 字节字段读**（`fd_capacity`），**禁止**在栈上分配大 hdr。

### 4.1 `linux_fd_entry_t`（`include/linux_compat/fs/linux_fd_table.h`）

```c
typedef struct linux_fd_entry {
    linux_fd_kind_t kind;       /* NONE, CONSOLE_*, VFS, PIPE */
    u32 vfs_handle;
    bool is_dir;
    bool pipe_read;
    char vfs_abs_path[LINUX_VFS_PATH_MAX];  /* openat 时的 abs path；目录 fd 的 dirfd base */
} linux_fd_entry_t;
```

- 初始容量 **`LINUX_FS_FD_INIT_CAP` = 128**；`linux_fd_alloc` 不足时 **+64** 扩容（`page_slice_set_size` + 新页 insert）。
- **`LINUX_VFS_PATH_MAX`** = **`VFS_PATH_MAX`** (256)。
- 目录 fd 路径 **只存于 entry.vfs_abs_path**（已移除 hdr 内 `dir_paths[]` 冗余侧表）。

### 4.2 Server handle（不变）

见 `servers/fs/vfs_handle.h`：`vfs_open_handle_t`，`refcnt`，dup/fork **RETAIN**。

---

## 5. 生命周期

| 事件 | fd 表 / slice |
|------|----------------|
| `gen_task_from_elf` / attach | `linux_fs_table_create`：预映射 slice 页，写 hdr + fd 0/1/2 |
| `fork` | `page_slice_clone` 整表 + 唯一 handle/pipe **RETAIN**（不再 attach→init→destroy→clone） |
| `execve` | `linux_fs_proc_reset`：release + **重建** slice |
| `sys_exit` | `linux_fs_proc_release_for_exit`：**仅** release 资源，**不** insert/rebuild slice |
| `delete_task` fini | `linux_fs_proc_destroy`：release + `page_slice_destroy` |

**page_slice_insert_page 何时出现？**  
- 新进程 attach / exec 重建 / fd 表扩容 — **不是** exit 路径。  
测例 harness 若在 THREAD_REAP cookie 后立即 spawn 下一 ELF，可能与上一进程 `del_vspace` **并发**；runner 已加 `find_task_by_pid` 等待 task 消失。

---

## 6. Syscall 要点

- **路径**：compat `linux_vfs_resolve_path`（cwd + dirfd → abs）；server 只收 abs path。
- **getcwd**：读 slice hdr 的 `cwd`（无 RPC）。
- **openat 目录 fd**：`ent.is_dir` + `ent.vfs_abs_path`。
- **scratch API**：`linux_fd_get` / `linux_fs_cwd` / `linux_fs_dir_path_lookup` 返回 per-CPU 临时指针，**下次同类调用前有效**；需长期持有请 `strncpy` 拷贝。

---

## 7. 与 page_slice 文件 ingest 的区分

| 用途 | slice 生命周期 | 内容 |
|------|----------------|------|
| **fd 表** | 与进程同生命周期 | hdr + fd 条目 |
| **exec/manifest/ELF** | load → map → **destroy** | 文件字节 |

用户态 `open/read` 仍走 **server handle**，fd 条目 **不** 绑定文件内容 slice（见 `VFS_EVOLUTION.md` F1–F3 待决）。

---

## 8. 验证门

```bash
make ARCH=x86_64 user && make ARCH=x86_64 build && make ARCH=x86_64 run
make ARCH=aarch64 user && make ARCH=aarch64 build && make ARCH=aarch64 run
```

关注：dup/dup2/pipe、openat(dirfd)、getcwd、fork、exit 后下一测例、52/52 harness。

---

## 9. 追溯

| 日期 | 事件 |
|------|------|
| 2026-07-09 | 方案 B 定稿 |
| 2026-07-12 | fd 表迁入 page_slice；移除 hdr.dir_paths；修复栈溢出；exit 不重建 slice |

改 fd 表时：**先更新本文 + VFS_EVOLUTION + VFS_IMPLEMENTATION_STATUS**。
