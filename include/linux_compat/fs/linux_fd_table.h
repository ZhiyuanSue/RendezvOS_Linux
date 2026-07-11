#ifndef _LINUX_COMPAT_FS_LINUX_FD_TABLE_H_
#define _LINUX_COMPAT_FS_LINUX_FD_TABLE_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>

#define LINUX_VFS_PATH_MAX   256
#define LINUX_FD_MAX         128
#define LINUX_DIR_PATH_SLOTS 4

#define LINUX_AT_FDCWD (-100)

#define LINUX_VFS_OPEN_IS_DIR_BIT (1LL << 31)
#define LINUX_VFS_OPEN_HANDLE_MASK ((1LL << 31) - 1LL)

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
} linux_fd_entry_t;

typedef struct linux_dir_path_slot {
        u8 fd;
        char path[LINUX_VFS_PATH_MAX];
} linux_dir_path_slot_t;

typedef struct linux_fs_state {
        char cwd[LINUX_VFS_PATH_MAX];
        linux_fd_entry_t fds[LINUX_FD_MAX];
        linux_dir_path_slot_t dir_paths[LINUX_DIR_PATH_SLOTS];
} linux_fs_state_t;

linux_fs_state_t *linux_fs_state(Tcb_Base *task);

error_t linux_fs_proc_attach(Tcb_Base *task);
void linux_fs_proc_reset(Tcb_Base *task);
void linux_fs_proc_destroy(Tcb_Base *task);
error_t linux_fs_proc_fork(Tcb_Base *child, Tcb_Base *parent);

i64 linux_vfs_resolve_path(Tcb_Base *task, i32 dirfd, const char *path,
                           char *out, u64 out_cap);

i32 linux_fd_alloc(Tcb_Base *task, const linux_fd_entry_t *ent);
linux_fd_entry_t *linux_fd_get(Tcb_Base *task, i32 fd);
i64 linux_fd_close(Tcb_Base *task, i32 fd);
i64 linux_fd_dup2(Tcb_Base *task, i32 oldfd, i32 newfd);

bool linux_fs_handle_in_use(const linux_fs_state_t *fs, u32 handle);

i64 linux_fs_dir_path_assign(linux_fs_state_t *fs, i32 fd, const char *path);
const char *linux_fs_dir_path_lookup(linux_fs_state_t *fs, i32 fd);
void linux_fs_dir_path_dup(linux_fs_state_t *fs, i32 oldfd, i32 newfd);

#endif /* _LINUX_COMPAT_FS_LINUX_FD_TABLE_H_ */
