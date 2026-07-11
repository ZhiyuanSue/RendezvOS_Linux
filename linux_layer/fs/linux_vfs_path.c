/*
 * Path normalize/join for compat-layer vfs_resolve (mirrors
 * servers/fs/vfs_path.c).
 */

#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/errno.h>

#include <common/string.h>

void linux_vfs_path_normalize(const char *in, char *out, u64 out_cap)
{
        const char *src = in;
        u64 i = 0;

        if (!out || out_cap == 0) {
                return;
        }

        if (!src) {
                src = "";
        }

        if (src[0] == '.' && src[1] == '/') {
                src += 2;
        } else if (src[0] == '.' && src[1] == '\0') {
                src += 1;
        }

        if (src[0] == '/') {
                src += 1;
        }

        out[0] = '/';
        i = 1;

        while (*src != '\0' && i + 1 < out_cap) {
                out[i++] = *src++;
        }

        out[i] = '\0';

        if (strcmp_s(out, "/", 2) == 0) {
                out[0] = '/';
                out[1] = '\0';
        }
}

static bool linux_vfs_path_join(const char *base, const char *rel, char *out,
                                u64 out_cap)
{
        char base_norm[LINUX_VFS_PATH_MAX];
        char norm[LINUX_VFS_PATH_MAX];
        const char *r = rel;
        u64 i;
        u64 j;

        if (!base || !rel || !out || out_cap == 0) {
                return false;
        }

        if (rel[0] == '/') {
                linux_vfs_path_normalize(rel, out, out_cap);
                return true;
        }

        linux_vfs_path_normalize(base, base_norm, sizeof(base_norm));

        if (r[0] == '.' && r[1] == '/') {
                r += 2;
        } else if (r[0] == '.' && r[1] == '\0') {
                strncpy(out, base_norm, out_cap - 1);
                out[out_cap - 1] = '\0';
                return true;
        }

        i = 0;
        for (j = 0; base_norm[j] != '\0' && i + 1 < out_cap; j++) {
                out[i++] = base_norm[j];
        }

        if (i == 0) {
                return false;
        }

        if (out[i - 1] != '/' && i + 1 < out_cap) {
                out[i++] = '/';
        }

        for (j = 0; r[j] != '\0' && i + 1 < out_cap; j++) {
                out[i++] = r[j];
        }

        out[i] = '\0';
        linux_vfs_path_normalize(out, norm, sizeof(norm));
        strncpy(out, norm, out_cap - 1);
        out[out_cap - 1] = '\0';
        return true;
}

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
                linux_vfs_path_normalize(path, out, out_cap);
                return 0;
        }

        if (dirfd == LINUX_AT_FDCWD) {
                strncpy(base, fs->cwd, sizeof(base) - 1);
                base[sizeof(base) - 1] = '\0';
        } else {
                dent = linux_fd_get(task, dirfd);
                if (!dent) {
                        return -LINUX_EBADF;
                }
                if (dent->kind != LINUX_FD_VFS || !dent->is_dir) {
                        return -LINUX_ENOTDIR;
                }

                dir_path = linux_fs_dir_path_lookup(fs, dirfd);
                if (!dir_path || dir_path[0] == '\0') {
                        return -LINUX_EBADF;
                }

                strncpy(base, dir_path, sizeof(base) - 1);
                base[sizeof(base) - 1] = '\0';
        }

        if (!linux_vfs_path_join(base, path, joined, sizeof(joined))) {
                return -LINUX_EINVAL;
        }

        linux_vfs_path_normalize(joined, out, out_cap);
        return 0;
}
