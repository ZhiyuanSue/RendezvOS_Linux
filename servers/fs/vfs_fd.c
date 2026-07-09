/*
 * Per-pid fd table for vfs_server.
 */

#include "vfs_fd.h"

#include <common/string.h>
#include <linux_compat/errno.h>

typedef struct vfs_fd_table {
        bool in_use;
        pid_t pid;
        vfs_open_file_t files[VFS_FD_MAX];
} vfs_fd_table_t;

static vfs_fd_table_t vfs_fd_tables[VFS_FD_PID_SLOTS];

void vfs_fd_init(void)
{
        memset(vfs_fd_tables, 0, sizeof(vfs_fd_tables));
}

static vfs_fd_table_t *vfs_fd_table_find(pid_t pid)
{
        u32 i;

        for (i = 0; i < VFS_FD_PID_SLOTS; i++) {
                if (vfs_fd_tables[i].in_use && vfs_fd_tables[i].pid == pid) {
                        return &vfs_fd_tables[i];
                }
        }

        return NULL;
}

static vfs_fd_table_t *vfs_fd_table_get_or_alloc(pid_t pid)
{
        vfs_fd_table_t *t = vfs_fd_table_find(pid);
        u32 i;

        if (t) {
                return t;
        }

        for (i = 0; i < VFS_FD_PID_SLOTS; i++) {
                if (!vfs_fd_tables[i].in_use) {
                        vfs_fd_tables[i].in_use = true;
                        vfs_fd_tables[i].pid = pid;
                        memset(vfs_fd_tables[i].files,
                               0,
                               sizeof(vfs_fd_tables[i].files));
                        return &vfs_fd_tables[i];
                }
        }

        return NULL;
}

static i32 vfs_fd_alloc_slot(vfs_fd_table_t *t)
{
        i32 fd;

        for (fd = VFS_FD_MIN; fd < VFS_FD_MAX; fd++) {
                if (!t->files[fd].in_use) {
                        return fd;
                }
        }

        return -1;
}

i64 vfs_fd_open(pid_t pid, const vfs_inode_t *ino, i32 open_flags)
{
        vfs_fd_table_t *t;
        i32 fd;
        vfs_open_file_t *file;

        if (!ino || pid <= 0) {
                return -LINUX_EINVAL;
        }

        t = vfs_fd_table_get_or_alloc(pid);
        if (!t) {
                return -LINUX_EMFILE;
        }

        fd = vfs_fd_alloc_slot(t);
        if (fd < 0) {
                return -LINUX_EMFILE;
        }

        file = &t->files[fd];
        file->in_use = true;
        file->ino = *ino;
        file->offset = 0;
        file->open_flags = open_flags;
        return (i64)fd;
}

i64 vfs_fd_close(pid_t pid, i32 fd)
{
        vfs_fd_table_t *t;
        vfs_open_file_t *file;

        if (fd < VFS_FD_MIN || fd >= VFS_FD_MAX) {
                return -LINUX_EBADF;
        }

        t = vfs_fd_table_find(pid);
        if (!t) {
                return -LINUX_EBADF;
        }

        file = &t->files[fd];
        if (!file->in_use) {
                return -LINUX_EBADF;
        }

        memset(file, 0, sizeof(*file));
        return 0;
}

vfs_open_file_t *vfs_fd_get(pid_t pid, i32 fd)
{
        vfs_fd_table_t *t;

        if (fd < VFS_FD_MIN || fd >= VFS_FD_MAX) {
                return NULL;
        }

        t = vfs_fd_table_find(pid);
        if (!t) {
                return NULL;
        }

        if (!t->files[fd].in_use) {
                return NULL;
        }

        return &t->files[fd];
}

void vfs_fd_drop_pid(pid_t pid)
{
        vfs_fd_table_t *t = vfs_fd_table_find(pid);

        if (!t) {
                return;
        }

        memset(t, 0, sizeof(*t));
}
