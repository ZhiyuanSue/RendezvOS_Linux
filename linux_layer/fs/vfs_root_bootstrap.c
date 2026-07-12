#include <linux_compat/fs/vfs_root_bootstrap.h>
#include <linux_compat/initcall.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/smp/smp.h>
#include <rendezvos/task/tcb.h>

#include "../../servers/fs/vfs_backend.h"
#include "../../servers/fs/vfs_root.h"

extern char rootfs_cpio_start[];
extern char rootfs_cpio_end[];
extern enum cpu_status CPU_STATE;

error_t linux_vfs_root_ensure_init(void)
{
        u64 len = (u64)(rootfs_cpio_end - rootfs_cpio_start);

        if (len == 0) {
                return -E_IN_PARAM;
        }

        return vfs_root_ensure_init(rootfs_cpio_start, len);
}

void linux_vfs_wait_backends_ready(void)
{
        while (per_cpu(CPU_STATE, VFS_SERVICE_CPU_ID) != cpu_enable) {
                schedule(percpu(core_tm));
        }

        while (!vfs_backend_boot_io_ready()) {
                schedule(percpu(core_tm));
        }
}
