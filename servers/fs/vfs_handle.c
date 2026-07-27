/*
 * Global open-file handles for vfs_server (scheme B — no per-pid fd table).
 */

#include "vfs_handle.h"

#include <linux_compat/fs/vfs_path.h>

#include <common/string.h>
#include <linux_compat/errno.h>
#include <rendezvos/error.h>

#include "vfs_slice_table.h"

static vfs_slice_table_t vfs_handle_tab;

static vfs_open_handle_t *vfs_handle_at(u32 i)
{
        return (vfs_open_handle_t *)vfs_slice_table_ptr(&vfs_handle_tab, i);
}

error_t vfs_handle_init(void)
{
        u32 idx;
        error_t err;

        vfs_slice_table_destroy(&vfs_handle_tab);
        err = vfs_slice_table_init(&vfs_handle_tab, sizeof(vfs_open_handle_t),
                                   32);
        if (err != REND_SUCCESS) {
                return err;
        }

        /* Reserve slot 0 — VFS_HANDLE_INVALID. */
        err = vfs_slice_table_push_zero(&vfs_handle_tab, &idx);
        if (err != REND_SUCCESS) {
                vfs_slice_table_destroy(&vfs_handle_tab);
                return err;
        }
        return REND_SUCCESS;
}

static u32 vfs_handle_alloc_slot(void)
{
        u32 i;
        u32 n;
        u32 idx;
        error_t err;

        n = vfs_slice_table_count(&vfs_handle_tab);
        for (i = 1; i < n; i++) {
                vfs_open_handle_t *h = vfs_handle_at(i);

                if (h && !h->in_use) {
                        return i;
                }
        }

        err = vfs_slice_table_push_zero(&vfs_handle_tab, &idx);
        if (err != REND_SUCCESS) {
                return VFS_HANDLE_INVALID;
        }
        if (!vfs_handle_at(idx)) {
                vfs_slice_table_pop_last(&vfs_handle_tab);
                return VFS_HANDLE_INVALID;
        }

        return idx;
}

u32 vfs_handle_open(const vfs_inode_t *ino, i32 open_flags)
{
        u32 id;
        vfs_open_handle_t *h;

        if (!ino) {
                return VFS_HANDLE_INVALID;
        }

        id = vfs_handle_alloc_slot();
        if (id == VFS_HANDLE_INVALID) {
                return VFS_HANDLE_INVALID;
        }

        h = vfs_handle_at(id);
        if (!h) {
                return VFS_HANDLE_INVALID;
        }

        h->in_use = true;
        h->refcnt = 1;
        h->ino = *ino;
        h->offset = 0;
        h->open_flags = open_flags;
        return id;
}

vfs_open_handle_t *vfs_handle_get(u32 handle)
{
        vfs_open_handle_t *h;

        if (handle == 0) {
                return NULL;
        }

        h = vfs_handle_at(handle);
        if (!h || !h->in_use) {
                return NULL;
        }

        return h;
}

i64 vfs_handle_retain(u32 handle)
{
        vfs_open_handle_t *h = vfs_handle_get(handle);

        if (!h) {
                return -LINUX_EBADF;
        }

        if (h->refcnt >= 0xffffffffu) {
                return -LINUX_EMFILE;
        }

        h->refcnt++;
        return 0;
}

i64 vfs_handle_close(u32 handle)
{
        vfs_open_handle_t *h = vfs_handle_get(handle);

        if (!h) {
                return -LINUX_EBADF;
        }

        if (h->refcnt == 0) {
                return -LINUX_EBADF;
        }

        h->refcnt--;
        if (h->refcnt > 0) {
                return 0;
        }

        memset(h, 0, sizeof(*h));
        return 0;
}

static bool vfs_handle_path_under_mount(const char *handle_path,
                                        const char *mount_path, u64 mlen)
{
        if (!handle_path || !mount_path) {
                return false;
        }

        if (strcmp_s(handle_path, mount_path, VFS_PATH_MAX) == 0) {
                return true;
        }

        if (mlen == 0) {
                return false;
        }

        if (strcmp_s(handle_path, mount_path, (size_t)mlen) != 0) {
                return false;
        }

        return handle_path[mlen] == '/';
}

bool vfs_handle_busy_under_path(const char *path)
{
        char norm[VFS_PATH_MAX];
        u64 mlen;
        u32 i;
        u32 n;

        if (!path) {
                return false;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        mlen = strlen(norm);

        n = vfs_slice_table_count(&vfs_handle_tab);
        for (i = 1; i < n; i++) {
                vfs_open_handle_t *h = vfs_handle_at(i);

                if (!h || !h->in_use) {
                        continue;
                }

                if (vfs_path_is_root(norm)) {
                        return true;
                }

                if (vfs_handle_path_under_mount(h->ino.path, norm, mlen)) {
                        return true;
                }
        }

        return false;
}
