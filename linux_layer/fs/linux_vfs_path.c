/*
 * Compat-only: dirfd + cwd path resolution (uses shared vfs_path_*).
 */

#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/vfs_path.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/vfs_protocol.h>

#include <common/string.h>

i64 linux_vfs_resolve_path(Tcb_Base *task, i32 dirfd, const char *path,
                           char *out, u64 out_cap)
{
        linux_fs_state_t *fs;
        char base[LINUX_VFS_PATH_MAX];
        char joined[LINUX_VFS_PATH_MAX];
        linux_fd_entry_t *dent;
        const char *dir_path;

        if (!task || !path || !out || out_cap == 0) {
                return -LINUX_EINVAL;
        }

        fs = linux_fs_state(task);
        if (!fs) {
                return -LINUX_ESRCH;
        }

        if (path[0] == '/') {
                vfs_path_normalize(path, out, out_cap);
                return 0;
        }

        if (dirfd == LINUX_AT_FDCWD) {
                strncpy(base, linux_fs_cwd(fs), sizeof(base) - 1);
                base[sizeof(base) - 1] = '\0';
        } else {
                dent = linux_fd_get(task, dirfd);
                if (!dent) {
                        return -LINUX_EBADF;
                }
                if (dent->kind != LINUX_FD_VFS) {
                        return -LINUX_ENOTDIR;
                }
                if (!dent->is_dir) {
                        const char *probe = dent->vfs_abs_path;

                        if (probe[0] == '\0') {
                                probe = linux_fs_dir_path_lookup(fs, dirfd);
                        }
                        if (probe && probe[0] != '\0'
                            && vfs_ipc_request_response(
                                       KMSG_OP_VFS_VALIDATE_DIR,
                                       VFS_KMSG_FMT_VALIDATE_DIR,
                                       probe)
                                       == 0) {
                                linux_fd_set_is_dir(fs, dirfd, true);
                                if (dent->vfs_abs_path[0] == '\0') {
                                        linux_fd_set_vfs_abs_path(fs, dirfd,
                                                                  probe);
                                }
                                dent = linux_fd_get(task, dirfd);
                        }
                }
                if (!dent || !dent->is_dir) {
                        return -LINUX_ENOTDIR;
                }

                if (dent->vfs_abs_path[0] != '\0') {
                        dir_path = dent->vfs_abs_path;
                } else {
                        dir_path = linux_fs_dir_path_lookup(fs, dirfd);
                }
                if (!dir_path || dir_path[0] == '\0') {
                        return -LINUX_EBADF;
                }

                strncpy(base, dir_path, sizeof(base) - 1);
                base[sizeof(base) - 1] = '\0';
        }

        if (!vfs_path_join(base, path, joined, sizeof(joined))) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(joined, out, out_cap);
        return 0;
}
