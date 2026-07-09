# 文件加载：page_slice 统一路径

> **Status**: 现行（2026-07-09）  
> **Index**: [`README.md`](README.md) · [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) · [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md)  
> **Core 边界**: 灌 slice / 生命周期在 compat；`load_elf_to_vs` 与 `page_slice_copy_*` 在 core（见 [`core/docs/USING_CORE.md`](../../core/docs/USING_CORE.md)）

---

## 1. 原则

**compat 层不再提供「整文件连续 kmalloc」读路径。** 内核侧从文件得到可执行/可解析镜像时，统一产出 **`page_slice`**（每 pgoff 一页 owned kallocator page）。

原因：

- 单块 `m_alloc(file_size)` 受 buddy **2MiB** 上限约束；
- manifest / ELF / 将来更大 initramfs 对象应共用同一 ingest 模型。

---

## 2. 两条 transport，一个容器

| Transport | API | 数据源 | 实现位置 |
|-----------|-----|--------|----------|
| **CPIO middle layer** | `vfs_kern_read_file_slice` | `ino.u.cpio_data` 连续 kva | `servers/fs/vfs_kern_load.c` |
| **IPC VFS server** | `linux_vfs_read_file_for_exec_slice` | 用户 scratch + VFS READ IPC | `linux_layer/fs/vfs_exec_load.c` |
| **Embedded payload** | `linux_page_slice_copy_from_kva` | `_num_app` 表区间 | `linux_layer/fs/linux_exec_image.c` |

三条路径最终都是 **`page_slice`**，再交给 core `load_elf_to_vs` / `page_slice_copy_to_*`。

### `linux_page_slice_copy_from_kva`

compat 辅助函数：**源已在内核连续 kva** 时，按页 `m_alloc` + `memcpy` + `page_slice_insert_page`。  
用于 CPIO 指针与 embedded 区间，**不是** core 原语。

头文件：`include/linux_compat/mm/linux_page_slice_file.h`  
实现：`linux_layer/mm/linux_page_slice_file.c`

辅助：`linux_page_slice_file_base(slice)` → pgoff 0 的 kva（读 ehdr / 迭代 ELF 宏）。

---

## 3. 调用链（按场景）

### execve

```text
sys_execve
  → linux_exec_load_elf_slice        (linux_layer/fs/linux_exec_image.c)
       ① vfs_kern_read_file_slice     (CPIO)
       ② linux_vfs_read_file_for_exec_slice (IPC, /tests 回退)
       ③ linux_page_slice_copy_from_kva (embedded program_map)
  → linux_exec_elf_slice_valid
  → load_elf_to_vs (core)
  → page_slice_destroy (compat policy)
```

brk / mmap_hint：`linux_proc_set_heap_from_elf_load`（`linux_layer/proc/linux_exec_proc.c`）。

### 用户测例 harness

```text
linux_user_test_load_manifest
  → vfs_kern_read_file_slice("/tests/manifest")
  → 在 slice 上逐行解析（不整文件 kmalloc）

linux_spawn_and_wait_test_path
  → vfs_kern_read_file_slice(path)
  → gen_task_from_elf → linux_elf_init_handler → page_slice_destroy
```

### spawn / task_test（embedded）

```text
linux_page_slice_copy_from_kva(_num_app range)
  → gen_task_from_elf → linux_elf_init_handler
```

---

## 4. 已删除的 legacy API

| 已移除 | 替代 |
|--------|------|
| `vfs_kern_read_file` | `vfs_kern_read_file_slice` |
| `vfs_kern_free_buffer` | `page_slice_destroy` |
| `linux_vfs_read_file` | `linux_vfs_read_file_slice` |
| `linux_vfs_read_file_for_exec` | `linux_vfs_read_file_for_exec_slice` |

---

## 5. 与 VFS open/read 的关系

- **测例数据**（`./text.txt`、`./mnt/`）：用户态 `open/read` → VFS server → cpio/ramfs 后端（**不经过 page_slice**）。
- **内核加载 ELF/manifest**：middle layer 或 IPC **直接读入 slice**，不经 fd 表。

详见 [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) §8（测例程序 vs 测例数据）。
