#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/time/linux_time_types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

static void linux_uname_fill(linux_utsname_t *name)
{
        const char *machine;

#if defined(_X86_64_)
        machine = "x86_64";
#elif defined(_AARCH64_)
        machine = "aarch64";
#else
        machine = "unknown";
#endif

        memset(name, 0, sizeof(*name));
        strncpy(name->sysname, "Linux", sizeof(name->sysname) - 1);
        strncpy(name->nodename, "rendezvos", sizeof(name->nodename) - 1);
        strncpy(name->release, "6.0.0-rendezvos", sizeof(name->release) - 1);
        strncpy(name->version, "#1 SMP RendezvOS", sizeof(name->version) - 1);
        strncpy(name->machine, machine, sizeof(name->machine) - 1);
        strncpy(name->domainname, "(none)", sizeof(name->domainname) - 1);
}

i64 sys_uname(u64 user_buf)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        linux_utsname_t name;
        error_t e;

        if (!user_buf) {
                return -LINUX_EFAULT;
        }
        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        linux_uname_fill(&name);
        e = linux_mm_store_to_user(vs, user_buf, &name, sizeof(name));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return 0;
}
