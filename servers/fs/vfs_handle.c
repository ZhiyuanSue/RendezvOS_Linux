/*
 * Global open-file handles for vfs_server (scheme B — no per-pid fd table).
 */

#include "vfs_handle.h"

#include <linux_compat/fs/vfs_path.h>

#include <common/string.h>
#include <linux_compat/errno.h>

static vfs_open_handle_t vfs_handles[VFS_HANDLE_MAX];

void vfs_handle_init(void)
{
        memset(vfs_handles, 0, sizeof(vfs_handles));
}

static u32 vfs_handle_alloc_slot(void)
{
        u32 i;

        for (i = 1; i < VFS_HANDLE_MAX; i++) {
                if (!vfs_handles[i].in_use) {
                        return i;
                }
        }

        return VFS_HANDLE_INVALID;
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

        h = &vfs_handles[id];
        h->in_use = true;
        h->refcnt = 1;
        h->ino = *ino;
        h->offset = 0;
        h->open_flags = open_flags;
        return id;
}

vfs_open_handle_t *vfs_handle_get(u32 handle)
{
        if (handle == 0 || handle >= VFS_HANDLE_MAX) {
                return NULL;
        }

        if (!vfs_handles[handle].in_use) {
                return NULL;
        }

        return &vfs_handles[handle];
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

        if (!path) {
                return false;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        mlen = strlen(norm);

        for (i = 1; i < VFS_HANDLE_MAX; i++) {
                if (!vfs_handles[i].in_use) {
                        continue;
                }

                if (vfs_path_is_root(norm)) {
                        return true;
                }

                if (vfs_handle_path_under_mount(vfs_handles[i].ino.path, norm,
                                                mlen)) {
                        return true;
                }
        }

        return false;
}
