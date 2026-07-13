#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

typedef struct {
        u16 ws_row;
        u16 ws_col;
        u16 ws_xpixel;
        u16 ws_ypixel;
} linux_winsize_t;

/* Linux asm-generic termios / tty ioctl numbers (same on x86_64 and aarch64). */
#define LINUX_TCGETS     0x5401U
#define LINUX_TIOCGWINSZ 0x5413U

i64 sys_ioctl(i32 fd, u32 request, u64 user_arg)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        error_t e;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }

        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (fd < 0) {
                return -LINUX_EBADF;
        }

        switch (request) {
        case LINUX_TIOCGWINSZ: {
                linux_winsize_t ws = {
                        .ws_row = 24,
                        .ws_col = 80,
                        .ws_xpixel = 0,
                        .ws_ypixel = 0,
                };

                if (!user_arg) {
                        return -LINUX_EFAULT;
                }
                e = linux_mm_store_to_user(vs, user_arg, &ws, sizeof(ws));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
                return 0;
        }
        case LINUX_TCGETS:
                /*
                 * glibc isatty() probes termios; UART/stdio are not ttys in
                 * this bring-up — ENOTTY matches pipe/socket behavior.
                 */
                return -LINUX_ENOTTY;
        default:
                return -LINUX_ENOTTY;
        }
}
