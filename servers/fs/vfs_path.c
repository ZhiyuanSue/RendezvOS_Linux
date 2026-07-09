/*
 * VFS path normalization (initramfs backends).
 */

#include "vfs_path.h"

#include <common/string.h>

void vfs_path_normalize(const char *in, char *out, u64 out_cap)
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

bool vfs_path_is_root(const char *path)
{
        char norm[VFS_PATH_MAX];

        if (!path) {
                return false;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        return norm[0] == '/' && norm[1] == '\0';
}

bool vfs_path_basename(const char *path, char *out, u64 out_cap)
{
        char norm[VFS_PATH_MAX];
        u64 len;
        u64 start;
        u64 i;

        if (!path || !out || out_cap == 0) {
                return false;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        if (vfs_path_is_root(norm)) {
                return false;
        }

        len = strlen(norm);
        start = len;

        while (start > 1 && norm[start - 1] != '/') {
                start--;
        }

        if (norm[start - 1] == '/') {
                /* keep start pointing at char after '/' */
        }

        for (i = 0; norm[start + i] != '\0' && i + 1 < out_cap; i++) {
                out[i] = norm[start + i];
        }

        if (i == 0) {
                return false;
        }

        out[i] = '\0';
        return true;
}

bool vfs_path_direct_child_name(const char *parent, const char *full,
                                char *name_out, u64 name_cap)
{
        char parent_norm[VFS_PATH_MAX];
        char full_norm[VFS_PATH_MAX];
        const char *suffix;
        u64 parent_len;
        u64 i;

        if (!parent || !full || !name_out || name_cap == 0) {
                return false;
        }

        vfs_path_normalize(parent, parent_norm, sizeof(parent_norm));
        vfs_path_normalize(full, full_norm, sizeof(full_norm));

        parent_len = strlen(parent_norm);
        for (i = 0; i < parent_len; i++) {
                if (full_norm[i] != parent_norm[i]) {
                        return false;
                }
        }
        if (full_norm[parent_len] != '\0' && full_norm[parent_len] != '/') {
                return false;
        }

        suffix = full_norm + parent_len;
        if (suffix[0] == '\0') {
                return false;
        }
        if (suffix[0] == '/') {
                suffix++;
        }
        if (suffix[0] == '\0') {
                return false;
        }

        for (i = 0; suffix[i] != '\0'; i++) {
                if (suffix[i] == '/') {
                        return false;
                }
        }

        if (i + 1 >= name_cap) {
                return false;
        }

        memcpy(name_out, suffix, i);
        name_out[i] = '\0';
        return true;
}

bool vfs_path_parent(const char *path, char *out, u64 out_cap)
{
        char norm[VFS_PATH_MAX];
        u64 len;
        u64 i;

        if (!path || !out || out_cap == 0) {
                return false;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        if (vfs_path_is_root(norm)) {
                return false;
        }

        len = strlen(norm);
        i = len;

        while (i > 1 && norm[i - 1] != '/') {
                i--;
        }

        if (i <= 1) {
                out[0] = '/';
                out[1] = '\0';
                return true;
        }

        if (i >= out_cap) {
                return false;
        }

        memcpy(out, norm, i);
        out[i] = '\0';
        return true;
}
