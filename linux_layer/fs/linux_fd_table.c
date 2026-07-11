/*
 * Per-process fd table (linux_layer) — scheme B in
 * doc/linux_compat/FD_TABLE.md.
 */

#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/linux_pipe.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/errno.h>

#include <common/string.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>

#define LINUX_DIR_PATH_FD_EMPTY ((u8)LINUX_FD_MAX)

static void linux_fs_dir_paths_clear_all(linux_fs_state_t *fs)
{
        u32 i;

        for (i = 0; i < LINUX_DIR_PATH_SLOTS; i++) {
                fs->dir_paths[i].fd = LINUX_DIR_PATH_FD_EMPTY;
                fs->dir_paths[i].path[0] = '\0';
        }
}

static i64 linux_fs_dir_path_store(linux_fs_state_t *fs, i32 fd,
                                   const char *path)
{
        u32 i;

        if (!fs || fd < 0 || fd >= (i32)LINUX_FD_MAX || !path) {
                return -LINUX_EINVAL;
        }

        for (i = 0; i < LINUX_DIR_PATH_SLOTS; i++) {
                if (fs->dir_paths[i].fd == (u8)fd) {
                        strncpy(fs->dir_paths[i].path,
                                path,
                                sizeof(fs->dir_paths[i].path) - 1);
                        fs->dir_paths[i]
                                .path[sizeof(fs->dir_paths[i].path) - 1] = '\0';
                        return 0;
                }
        }

        for (i = 0; i < LINUX_DIR_PATH_SLOTS; i++) {
                if (fs->dir_paths[i].fd == LINUX_DIR_PATH_FD_EMPTY) {
                        fs->dir_paths[i].fd = (u8)fd;
                        strncpy(fs->dir_paths[i].path,
                                path,
                                sizeof(fs->dir_paths[i].path) - 1);
                        fs->dir_paths[i]
                                .path[sizeof(fs->dir_paths[i].path) - 1] = '\0';
                        return 0;
                }
        }

        return -LINUX_EMFILE;
}

static void linux_fs_dir_path_release(linux_fs_state_t *fs, i32 fd)
{
        u32 i;

        if (!fs || fd < 0 || fd >= (i32)LINUX_FD_MAX) {
                return;
        }

        for (i = 0; i < LINUX_DIR_PATH_SLOTS; i++) {
                if (fs->dir_paths[i].fd == (u8)fd) {
                        fs->dir_paths[i].fd = LINUX_DIR_PATH_FD_EMPTY;
                        fs->dir_paths[i].path[0] = '\0';
                        return;
                }
        }
}

const char *linux_fs_dir_path_lookup(linux_fs_state_t *fs, i32 fd)
{
        u32 i;

        if (!fs || fd < 0 || fd >= (i32)LINUX_FD_MAX) {
                return NULL;
        }

        for (i = 0; i < LINUX_DIR_PATH_SLOTS; i++) {
                if (fs->dir_paths[i].fd == (u8)fd) {
                        return fs->dir_paths[i].path;
                }
        }

        return NULL;
}

i64 linux_fs_dir_path_assign(linux_fs_state_t *fs, i32 fd, const char *path)
{
        return linux_fs_dir_path_store(fs, fd, path);
}

void linux_fs_dir_path_dup(linux_fs_state_t *fs, i32 oldfd, i32 newfd)
{
        const char *path;

        if (!fs || oldfd == newfd) {
                return;
        }

        linux_fs_dir_path_release(fs, newfd);
        path = linux_fs_dir_path_lookup(fs, oldfd);
        if (path) {
                (void)linux_fs_dir_path_store(fs, newfd, path);
        }
}

linux_fs_state_t *linux_fs_state(Tcb_Base *task)
{
        linux_proc_append_t *pa;

        if (!task) {
                return NULL;
        }

        pa = linux_proc_append(task);
        if (!pa) {
                return NULL;
        }

        return pa->fs;
}

static linux_fs_state_t *linux_fs_alloc_state(void)
{
        struct allocator *alloc = percpu(kallocator);
        linux_fs_state_t *fs;

        if (!alloc) {
                return NULL;
        }

        fs = (linux_fs_state_t *)alloc->m_alloc(alloc, sizeof(*fs));
        return fs;
}

static void linux_fs_free_state(linux_fs_state_t *fs)
{
        struct allocator *alloc = percpu(kallocator);

        if (!fs || !alloc) {
                return;
        }

        alloc->m_free(alloc, fs);
}

static void linux_fs_init_state(linux_fs_state_t *fs)
{
        u32 i;

        if (!fs) {
                return;
        }

        fs->cwd[0] = '/';
        fs->cwd[1] = '\0';
        linux_fs_dir_paths_clear_all(fs);

        for (i = 0; i < LINUX_FD_MAX; i++) {
                fs->fds[i].kind = LINUX_FD_NONE;
                fs->fds[i].vfs_handle = 0;
                fs->fds[i].is_dir = false;
                fs->fds[i].pipe_read = false;
        }

        fs->fds[0].kind = LINUX_FD_CONSOLE_IN;
        fs->fds[1].kind = LINUX_FD_CONSOLE_OUT;
        fs->fds[2].kind = LINUX_FD_CONSOLE_ERR;
}

error_t linux_fs_proc_attach(Tcb_Base *task)
{
        linux_proc_append_t *pa;
        linux_fs_state_t *fs;

        if (!task) {
                return -E_IN_PARAM;
        }

        pa = linux_proc_append(task);
        if (!pa) {
                return -E_IN_PARAM;
        }

        if (pa->fs) {
                linux_fs_init_state(pa->fs);
                return REND_SUCCESS;
        }

        fs = linux_fs_alloc_state();
        if (!fs) {
                return -E_RENDEZVOS;
        }

        linux_fs_init_state(fs);
        pa->fs = fs;
        return REND_SUCCESS;
}

static void linux_fs_release_vfs_handle(u32 handle)
{
        if (handle != 0) {
                (void)vfs_ipc_request_response(
                        KMSG_OP_VFS_CLOSE, VFS_KMSG_FMT_CLOSE, handle);
        }
}

static void linux_fs_retain_vfs_handle(u32 handle)
{
        if (handle != 0) {
                (void)vfs_ipc_request_response(KMSG_OP_VFS_HANDLE_RETAIN,
                                               VFS_KMSG_FMT_HANDLE_RETAIN,
                                               handle);
        }
}

static void linux_fs_reset_state(linux_fs_state_t *fs)
{
        u32 seen[VFS_HANDLE_MAX];
        u32 i;

        if (!fs) {
                return;
        }

        memset(seen, 0, sizeof(seen));

        for (i = 0; i < LINUX_FD_MAX; i++) {
                linux_fd_entry_t *ent = &fs->fds[i];

                if (ent->kind != LINUX_FD_VFS || ent->vfs_handle == 0) {
                        continue;
                }
                if (ent->vfs_handle < VFS_HANDLE_MAX
                    && !seen[ent->vfs_handle]) {
                        seen[ent->vfs_handle] = 1;
                        linux_fs_release_vfs_handle(ent->vfs_handle);
                }
        }

        linux_fs_init_state(fs);
}

void linux_fs_proc_reset(Tcb_Base *task)
{
        linux_fs_state_t *fs = linux_fs_state(task);

        if (!fs) {
                (void)linux_fs_proc_attach(task);
                return;
        }

        linux_fs_reset_state(fs);
}

void linux_fs_proc_destroy(Tcb_Base *task)
{
        linux_proc_append_t *pa;
        linux_fs_state_t *fs;

        if (!task) {
                return;
        }

        pa = linux_proc_append(task);
        if (!pa || !pa->fs) {
                return;
        }

        fs = pa->fs;
        linux_fs_reset_state(fs);
        linux_fs_free_state(fs);
        pa->fs = NULL;
}

static void linux_fs_fork_copy_state(linux_fs_state_t *child,
                                     const linux_fs_state_t *parent)
{
        u32 seen[VFS_HANDLE_MAX];
        u32 i;

        if (!child || !parent) {
                return;
        }

        *child = *parent;
        memset(seen, 0, sizeof(seen));

        for (i = 0; i < LINUX_FD_MAX; i++) {
                if (child->fds[i].kind != LINUX_FD_VFS
                    || child->fds[i].vfs_handle == 0) {
                        if (child->fds[i].kind == LINUX_FD_PIPE) {
                                linux_pipe_fork_retain(child->fds[i].vfs_handle);
                        }
                        continue;
                }
                if (child->fds[i].vfs_handle < VFS_HANDLE_MAX
                    && !seen[child->fds[i].vfs_handle]) {
                        seen[child->fds[i].vfs_handle] = 1;
                        linux_fs_retain_vfs_handle(child->fds[i].vfs_handle);
                }
        }
}

error_t linux_fs_proc_fork(Tcb_Base *child, Tcb_Base *parent)
{
        linux_fs_state_t *parent_fs;
        linux_fs_state_t *child_fs;
        error_t e;

        if (!child || !parent) {
                return -E_IN_PARAM;
        }

        parent_fs = linux_fs_state(parent);
        e = linux_fs_proc_attach(child);
        if (e != REND_SUCCESS) {
                return e;
        }

        child_fs = linux_fs_state(child);
        if (!child_fs) {
                return -E_RENDEZVOS;
        }

        if (parent_fs) {
                linux_fs_fork_copy_state(child_fs, parent_fs);
        } else {
                linux_fs_init_state(child_fs);
        }

        return REND_SUCCESS;
}

bool linux_fs_handle_in_use(const linux_fs_state_t *fs, u32 handle)
{
        u32 i;

        if (!fs || handle == 0) {
                return false;
        }

        for (i = 0; i < LINUX_FD_MAX; i++) {
                if (fs->fds[i].kind == LINUX_FD_VFS
                    && fs->fds[i].vfs_handle == handle) {
                        return true;
                }
        }

        return false;
}

linux_fd_entry_t *linux_fd_get(Tcb_Base *task, i32 fd)
{
        linux_fs_state_t *fs;

        if (fd < 0 || fd >= (i32)LINUX_FD_MAX) {
                return NULL;
        }

        fs = linux_fs_state(task);
        if (!fs) {
                return NULL;
        }

        if (fs->fds[fd].kind == LINUX_FD_NONE) {
                return NULL;
        }

        return &fs->fds[fd];
}

i32 linux_fd_alloc(Tcb_Base *task, const linux_fd_entry_t *ent)
{
        linux_fs_state_t *fs;
        i32 fd;

        if (!task || !ent) {
                return -1;
        }

        fs = linux_fs_state(task);
        if (!fs) {
                return -1;
        }

        for (fd = 0; fd < (i32)LINUX_FD_MAX; fd++) {
                if (fs->fds[fd].kind == LINUX_FD_NONE) {
                        fs->fds[fd] = *ent;
                        return fd;
                }
        }

        return -1;
}

i64 linux_fd_close(Tcb_Base *task, i32 fd)
{
        linux_fs_state_t *fs;
        linux_fd_entry_t *ent;
        u32 handle;

        if (fd < 0 || fd >= (i32)LINUX_FD_MAX) {
                return -LINUX_EBADF;
        }

        fs = linux_fs_state(task);
        if (!fs) {
                return -LINUX_ESRCH;
        }

        ent = &fs->fds[fd];
        if (ent->kind == LINUX_FD_NONE) {
                return -LINUX_EBADF;
        }

        handle = ent->vfs_handle;
        if (ent->kind == LINUX_FD_PIPE) {
                linux_pipe_fd_closed(handle, ent->pipe_read);
        } else if (ent->kind == LINUX_FD_VFS) {
                if (ent->is_dir) {
                        linux_fs_dir_path_release(fs, fd);
                }
                ent->kind = LINUX_FD_NONE;
                ent->vfs_handle = 0;
                ent->is_dir = false;
                ent->pipe_read = false;
                if (handle != 0 && !linux_fs_handle_in_use(fs, handle)) {
                        linux_fs_release_vfs_handle(handle);
                }
                return 0;
        }

        ent->kind = LINUX_FD_NONE;
        ent->vfs_handle = 0;
        ent->is_dir = false;
        ent->pipe_read = false;

        return 0;
}

i64 linux_fd_dup2(Tcb_Base *task, i32 oldfd, i32 newfd)
{
        linux_fs_state_t *fs;
        linux_fd_entry_t *oldent;
        linux_fd_entry_t *newent;
        u32 replaced_handle;

        if (oldfd < 0 || oldfd >= (i32)LINUX_FD_MAX || newfd < 0
            || newfd >= (i32)LINUX_FD_MAX) {
                return -LINUX_EBADF;
        }

        if (oldfd == newfd) {
                if (!linux_fd_get(task, oldfd)) {
                        return -LINUX_EBADF;
                }
                return (i64)newfd;
        }

        fs = linux_fs_state(task);
        if (!fs) {
                return -LINUX_ESRCH;
        }

        oldent = linux_fd_get(task, oldfd);
        if (!oldent) {
                return -LINUX_EBADF;
        }

        newent = &fs->fds[newfd];
        replaced_handle = 0;
        if (newent->kind == LINUX_FD_VFS) {
                replaced_handle = newent->vfs_handle;
        }

        *newent = *oldent;

        if (newent->is_dir) {
                linux_fs_dir_path_dup(fs, oldfd, newfd);
        } else {
                linux_fs_dir_path_release(fs, newfd);
        }

        if (newent->kind == LINUX_FD_VFS && newent->vfs_handle != 0) {
                linux_fs_retain_vfs_handle(newent->vfs_handle);
        } else if (newent->kind == LINUX_FD_PIPE) {
                linux_pipe_fork_retain(newent->vfs_handle);
        }

        if (replaced_handle != 0
            && !linux_fs_handle_in_use(fs, replaced_handle)) {
                linux_fs_release_vfs_handle(replaced_handle);
        }

        return (i64)newfd;
}
