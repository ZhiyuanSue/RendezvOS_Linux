#include <common/stdbool.h>
#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <syscall.h>

#if defined(_AARCH64_)
#include <arch/aarch64/boot/arch_setup.h>
#else
#include <arch/x86_64/boot/arch_setup.h>
#endif

/*
 * Stdout/stderr bytes use `log_put_locked()` — same UART path as `printk`
 * / `log.c`, with SMP serialization compatible with `pr_*`.
 *
 * Later: dispatch on a per-task fd table to VFS-backed `file` objects; fd 1/2
 * stay wired to this backend.
 */

#define LINUX_WRITE_MAX_CHUNK (4096u)
#define LINUX_WRITE_TOTAL_CAP (0x100000u) /* 1 MiB per syscall */

static bool user_buf_range_ok(u64 user_buf, u64 count)
{
        if (count == 0)
                return true;
        if (user_buf >= KERNEL_VIRT_OFFSET)
                return false;
        if (user_buf + count < user_buf)
                return false;
        if (user_buf + count > KERNEL_VIRT_OFFSET)
                return false;
        return true;
}

i64 sys_write(i32 fd, u64 user_buf, u64 count)
{
        if (count == 0)
                return 0;

        if (count > LINUX_WRITE_TOTAL_CAP)
                return -LINUX_EINVAL;

        if (fd != 1 && fd != 2)
                return -LINUX_EBADF;

        if (!user_buf_range_ok(user_buf, count))
                return -LINUX_EFAULT;

        u8 buf[LINUX_WRITE_MAX_CHUNK];
        u64 total = 0;

        while (total < count) {
                u64 chunk = count - total;
                if (chunk > LINUX_WRITE_MAX_CHUNK)
                        chunk = LINUX_WRITE_MAX_CHUNK;
                memcpy(buf, (const void *)(user_buf + total), chunk);
                log_put_locked(buf, chunk);
                total += chunk;
        }

        return (i64)count;
}
