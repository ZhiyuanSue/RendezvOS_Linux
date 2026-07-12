#ifndef _LINUX_COMPAT_FS_LINUX_FD_TABLE_H_
#define _LINUX_COMPAT_FS_LINUX_FD_TABLE_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/fs/vfs_path.h>
#include <linux_compat/fs/vfs_path.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/sync/cas_lock.h>
#include <rendezvos/task/tcb.h>

#define LINUX_VFS_PATH_MAX   VFS_PATH_MAX

/*
 * Initial fd slot count when a process fs table is created. The table lives in
 * a page_slice and may grow via linux_fd_alloc; this is not a hard upper bound.
 */
#define LINUX_FS_FD_INIT_CAP 128

#define LINUX_AT_FDCWD (-100)

typedef enum linux_fd_kind {
        LINUX_FD_NONE = 0,
        LINUX_FD_CONSOLE_IN,
        LINUX_FD_CONSOLE_OUT,
        LINUX_FD_CONSOLE_ERR,
        LINUX_FD_VFS,
        LINUX_FD_PIPE,
} linux_fd_kind_t;

typedef struct linux_fd_entry {
        linux_fd_kind_t kind;
        u32 vfs_handle;
        bool is_dir;
        bool pipe_read;
        char vfs_abs_path[LINUX_VFS_PATH_MAX];
} linux_fd_entry_t;

typedef struct linux_fs_state {
        struct page_slice *table;
        cas_lock_t lock;
} linux_fs_state_t;

/*
 * linux_fd_get / linux_fs_cwd / linux_fs_dir_path_lookup return per-CPU scratch
 * pointers valid until the next call on the same CPU; copy if retained.
 */

linux_fs_state_t *linux_fs_state(Tcb_Base *task);

error_t linux_fs_proc_attach(Tcb_Base *task);
void linux_fs_proc_reset(Tcb_Base *task);
void linux_fs_proc_release_for_exit(Tcb_Base *task);
void linux_fs_proc_destroy(Tcb_Base *task);
error_t linux_fs_proc_fork(Tcb_Base *child, Tcb_Base *parent);

u32 linux_fs_fd_capacity(const linux_fs_state_t *fs);
const char *linux_fs_cwd(const linux_fs_state_t *fs);
void linux_fs_set_cwd(linux_fs_state_t *fs, const char *cwd);

i64 linux_vfs_resolve_path(Tcb_Base *task, i32 dirfd, const char *path,
                           char *out, u64 out_cap);

i32 linux_fd_alloc(Tcb_Base *task, const linux_fd_entry_t *ent);
i32 linux_fd_lowest_free(Tcb_Base *task);
linux_fd_entry_t *linux_fd_get(Tcb_Base *task, i32 fd);
i64 linux_fd_close(Tcb_Base *task, i32 fd);
i64 linux_fd_dup2(Tcb_Base *task, i32 oldfd, i32 newfd);

bool linux_fs_handle_in_use(const linux_fs_state_t *fs, u32 handle);

i64 linux_fs_dir_path_assign(linux_fs_state_t *fs, i32 fd, const char *path);
const char *linux_fs_dir_path_lookup(linux_fs_state_t *fs, i32 fd);
void linux_fs_dir_path_dup(linux_fs_state_t *fs, i32 oldfd, i32 newfd);
void linux_fd_set_vfs_abs_path(linux_fs_state_t *fs, i32 fd, const char *path);
void linux_fd_set_is_dir(linux_fs_state_t *fs, i32 fd, bool is_dir);

#endif /* _LINUX_COMPAT_FS_LINUX_FD_TABLE_H_ */
