# Stdio shim（`write` 先行，无 VFS）

## 目的

在用户态测试与极简 libc 场景下，进程需要向 **stdout（fd 1）** 与 **stderr（fd 2）** 输出；在完整 **VFS / 打开文件表** 落地之前，用一层 **可替换的后端** 满足 `write`，避免在 `syscall_entry.c` 里写死 `printk` 且便于后续接真文件系统。

## 当前语义

| 项目 | 说明 |
|------|------|
| 实现 | `linux_layer/io/sys_write.c` 中 `sys_write` |
| 已支持 fd | **仅 1、2**（与 Linux 约定一致） |
| 未支持 | **stdin（fd 0）**、`read`、管道、普通文件 |
| 错误 | 其他 fd 返回 `-EBADF`（`LINUX_EBADF`）；非法 `buf`/`count` 返回 `-EFAULT` / `-EINVAL`（见 `include/linux_compat/errno.h`） |
| 输出路径 | **`core/modules/log/log.c`**：用户态 `write` 走 `log_put_locked()`；`printk` 内部走 `log_put_byte()`（通过 `putc_char`）。默认只走 **`uart_putc`**（具体后端由 `_UART_16550A_` / `_UART_PL011_` 等编译选项与 [`uart.c`](../../core/modules/driver/uart/uart.c) 决定）。SMP 下 `write` 路径复用 [`log`](../../core/include/modules/log/log.h) 的 **同一 MCS 锁**，减少与 `pr_*` 格式化输出交错。 |

## 启动与 UART（不要动 `cmain` 开头）

早期可见输出依赖 [`core/kernel/main.c`](../../core/kernel/main.c) 里 **`cmain()` 最前面的 `uart_open(...)` → `log_init(...)`** 顺序；**本 shim 不修改该启动路径**。`log_put_*` 只 **使用**已在启动阶段打开/映射好的 UART 后端（与 `printk` 相同），不在 `write` 里重复初始化串口。

## 用户指针

当前在 **用户 CR3 仍指向该任务地址空间** 的前提下，用 `memcpy` 从用户虚拟地址读入内核栈上的块缓冲区；**尚未**实现完整的 `copy_from_user`（页表探测 + 缺页）。后续应在 `linux_layer/mm/` 或 core 导出接口中补齐，再改 `sys_write` 的拷贝路径。

## 后续接 VFS 的扩展点

1. **按 fd 分发**：引入每进程（或每线程组）的 **fd 表**（或 `struct file*` 表），`sys_write` 仅做参数检查与分发；fd 1/2 绑定到本 shim 或终端驱动，其余 fd 走 inode/管道。
2. **本模块定位**：fd 1/2 继续调用 **`log_put_locked` / 未来的 fd 后端**；不要把用户缓冲直接丢给 `printk("%s")`（避免格式化、颜色与日志级别污染用户字节流）。更复杂的 **多路串口 / virtio-console** 建议在 core 侧扩展 log 的输出 sink（例如注册额外 sink），仍保持 `cmain` 的早期 `uart_open` 不变。

## 相关文档

- 系统调用总清单与顺序：[`SYSCALLS.md`](SYSCALLS.md)
- Linux 兼容 errno 约定：[`doc/ai/INVARIANTS.md`](../ai/INVARIANTS.md)（Linux compatibility 小节）
