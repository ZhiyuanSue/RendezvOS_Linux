#ifndef _LINUX_COMPAT_INITCALL_H_
#define _LINUX_COMPAT_INITCALL_H_

#include <common/stdbool.h>
#include <rendezvos/smp/percpu.h>

extern cpu_id_t BSP_ID;

/*
 * SMP note: core runs do_init_call() on BSP before start_smp(), then again on
 * each AP after all CPUs are online. Global singleton setup must use
 * linux_init_bsp_once(); per-CPU registration (trap vectors, server threads)
 * runs on every CPU.
 */
static inline bool linux_init_on_bsp(void)
{
        return percpu(cpu_number) == BSP_ID;
}

static inline bool linux_init_bsp_once(bool *done)
{
        if (!linux_init_on_bsp() || *done)
                return false;
        return true;
}

static inline void linux_init_bsp_mark_done(bool *done)
{
        *done = true;
}

/* VFS server + storage backend IPC threads (see servers/fs/). */
#define VFS_SERVICE_CPU_ID 1u

static inline bool linux_init_on_vfs_service_cpu(void)
{
        return percpu(cpu_number) == VFS_SERVICE_CPU_ID;
}

static inline bool linux_init_vfs_service_once(bool *done)
{
        if (!linux_init_on_vfs_service_cpu() || *done)
                return false;
        return true;
}

static inline void linux_init_vfs_service_mark_done(bool *done)
{
        *done = true;
}

#endif
