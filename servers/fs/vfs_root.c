/*
 * VFS root mount: cpio + namespace bring-up only.
 * Path ops call vfs_namespace_* directly.
 */

#include "cpio_rofs.h"
#include "vfs_namespace.h"
#include "vfs_root.h"

#include <modules/log/log.h>
#include <rendezvos/error.h>

static bool vfs_root_initialized;

error_t vfs_root_init(const void *cpio_image, u64 cpio_len)
{
        error_t err;

        if (vfs_root_initialized) {
                return REND_SUCCESS;
        }
        if (!cpio_image || cpio_len == 0) {
                return -E_IN_PARAM;
        }

        vfs_namespace_reset();

        err = cpio_rofs_init(cpio_image, cpio_len);
        if (err != REND_SUCCESS) {
                pr_error("[VFS] cpio_rofs_init failed: %d\n", (int)err);
                return err;
        }

        err = vfs_namespace_init();
        if (err != REND_SUCCESS) {
                pr_error(
                        "[VFS] vfs_namespace_init failed: %d (cpio %u entries)\n",
                        (int)err,
                        cpio_rofs_parsed_count());
                return err;
        }

        vfs_root_initialized = true;

        pr_info("[VFS] root ready: cpio %u entries, namespace %u entries\n",
                cpio_rofs_parsed_count(),
                vfs_namespace_count());
        return REND_SUCCESS;
}
