# Stdio 与 fd 表（方案 B 后）

## 当前语义

| 项目 | 说明 |
|------|------|
| fd 表 | **`linux_proc_append_t.fs`** — 0/1/2 预装 console，`3+` 为 VFS 或空 |
| `write` | 查 fd 表：`CONSOLE_OUT/ERR` → `sys_write_impl` → `log_put_locked` / UART |
| `write` 重定向 | `dup2(vfs_fd, 1)` 后 fd 1 为 `LINUX_FD_VFS` → VFS IPC |
| `read(0)` | bootstrap 返回 **0（EOF）**；无 UART RX |
| 实现 | `linux_layer/fs/linux_fd_table.c` + `sys_fs_impl.c` |

## 设计文档

完整模型：[`FD_TABLE.md`](FD_TABLE.md)

## 启动

仍依赖 `cmain()` 内 `uart_open` → `log_init`；stdio shim **不**重复初始化 UART。

## 相关

- [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md)
- [`DECISIONS.md`](../ai/DECISIONS.md) — 2026-07-09 fd 表方案 B
