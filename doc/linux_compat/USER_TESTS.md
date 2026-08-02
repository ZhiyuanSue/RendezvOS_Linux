# Linux compat user tests（用户态套件）

本文件描述 **initramfs 里的用户态 ELF 测例**（`rootfs/tests/`）如何被编排与验证。  
**内核侧不再有 test harness**：PID1 启动在 `linux_layer/init/linux_boot.c`。

**相关**: [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) · [`ROOTFS.md`](ROOTFS.md) · [`FILE_LOADING.md`](FILE_LOADING.md) · [`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md)

## Boot 与套件

1. `make user` 将静态 ELF 写入 `rootfs/tests/`，并生成 `manifest` + `run_all.sh`。
2. `make rootfs` 将 `rootfs/`（含 `/bin/busybox`、`/init`）打成 cpio 并链进内核。
3. BSP `linux_boot`：空 user task → **`linux_exec_replace_image("/init", argv)`**（与 `sys_execve` 同路径）→ 落入用户；argv=`sh /tests/run_all.sh`。
4. 套件在 **用户态 ash** 里顺序 `sys_execve` 各 ELF；成败看 exit code / stdout。

内核等待 `/init` 结束：`boot_wait_cookie` + `clean_server` → `linux_boot_notify_exit`（见 `boot_wait.h`）。

## 为什么需要 single / smp 分层（历史 / 可选）

- **single**：优先验证功能点，减少并发噪声。
- **smp**：验证并发下 allocator / 跨核清理 / 生命周期。

默认路径已是 busybox `run_all`；内核 per-CPU harness 循环已删除。

## Config 开关

- **Linux compat boot**：`script/config/root.json` → `LINUX_COMPAT_TEST`（名字历史遗留；实际打开的是 compat boot + 相关代码路径）
- **core tests**：`core/script/config/config_*.json` 的 `modules.test`

`do_init_call()` 在所有 CPU 上执行；boot 线程创建必须限制在 BSP（`linux_init_on_bsp()`）。

## 输出乱序

多核下用户态 stdout 可能交错；套件以 `run_all` 汇总 `pass=` / `fail=` 为准。
