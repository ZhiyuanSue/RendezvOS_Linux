#include <linux_compat/fs/vfs_root_bootstrap.h>
#include <rendezvos/error.h>

#include "../../servers/fs/vfs_root.h"

extern char rootfs_cpio_start[];
extern char rootfs_cpio_end[];

error_t linux_vfs_root_ensure_init(void)
{
        u64 len = (u64)(rootfs_cpio_end - rootfs_cpio_start);

        if (len == 0) {
                return -E_IN_PARAM;
        }

        return vfs_root_ensure_init(rootfs_cpio_start, len);
}
