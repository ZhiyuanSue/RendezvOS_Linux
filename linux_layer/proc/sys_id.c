#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

static linux_proc_append_t *linux_current_proc_append(void)
{
        Tcb_Base *t = get_cpu_current_task();

        return t ? linux_proc_append(t) : NULL;
}

i64 sys_getuid(void)
{
        linux_proc_append_t *pa = linux_current_proc_append();

        if (!pa) {
                return -LINUX_ESRCH;
        }
        return (i64)pa->uid;
}

i64 sys_getgid(void)
{
        linux_proc_append_t *pa = linux_current_proc_append();

        if (!pa) {
                return -LINUX_ESRCH;
        }
        return (i64)pa->gid;
}

i64 sys_setuid(u32 uid)
{
        linux_proc_append_t *pa = linux_current_proc_append();

        if (!pa) {
                return -LINUX_ESRCH;
        }
        if (pa->euid != 0 && uid != pa->uid) {
                return -LINUX_EPERM;
        }
        pa->uid = uid;
        pa->euid = uid;
        return 0;
}

i64 sys_setgid(u32 gid)
{
        linux_proc_append_t *pa = linux_current_proc_append();

        if (!pa) {
                return -LINUX_ESRCH;
        }
        if (pa->euid != 0 && gid != pa->gid) {
                return -LINUX_EPERM;
        }
        pa->gid = gid;
        pa->egid = gid;
        return 0;
}
