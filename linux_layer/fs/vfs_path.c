/*
 * Shared VFS path normalization (servers/fs + linux_layer compat).
 *
 * Stack discipline: core kstack is 2 pages (8 KiB).  Collapse keeps only
 * (offset,len) metadata pointing into caller scratch — no names[][] pool.
 * Results go to caller-provided @out; one 256B scratch per normalize frame.
 */

#include <linux_compat/fs/vfs_path.h>

#include <common/string.h>

#define VFS_PATH_MAX_COMPONENTS    32u
#define VFS_PATH_MAX_COMPONENT_LEN 64u

static bool vfs_path_is_root_norm(const char *norm)
{
        return norm && norm[0] == '/' && norm[1] == '\0';
}

static void vfs_path_collapse(const char *raw, char *out, u64 out_cap)
{
        u16 comp_off[VFS_PATH_MAX_COMPONENTS];
        u8 comp_len[VFS_PATH_MAX_COMPONENTS];
        u32 depth = 0;
        const char *src;

        if (!raw || !out || out_cap == 0) {
                return;
        }

        src = raw;
        if (src[0] == '/') {
                src++;
        }

        while (*src != '\0' && depth < VFS_PATH_MAX_COMPONENTS) {
                u64 clen = 0;

                while (src[clen] != '\0' && src[clen] != '/') {
                        clen++;
                }

                if (clen > 0) {
                        if (clen == 1 && src[0] == '.') {
                                /* skip */
                        } else if (clen == 2 && src[0] == '.'
                                   && src[1] == '.') {
                                if (depth > 0) {
                                        depth--;
                                }
                        } else {
                                u64 copy = clen;

                                if (copy >= VFS_PATH_MAX_COMPONENT_LEN) {
                                        copy = VFS_PATH_MAX_COMPONENT_LEN - 1;
                                }

                                comp_off[depth] = (u16)(src - raw);
                                comp_len[depth] = (u8)copy;
                                depth++;
                        }
                }

                src += clen;
                if (*src == '/') {
                        src++;
                }
        }

        if (depth == 0) {
                out[0] = '/';
                out[1] = '\0';
                return;
        }

        {
                u64 i = 0;
                u32 d;

                out[i++] = '/';
                for (d = 0; d < depth; d++) {
                        u64 j = 0;
                        const char *comp = raw + comp_off[d];

                        while (j < comp_len[d] && comp[j] != '\0'
                               && i + 1 < out_cap) {
                                out[i++] = comp[j++];
                        }
                        if (d + 1 < depth && i + 1 < out_cap) {
                                out[i++] = '/';
                        }
                }
                out[i] = '\0';
        }
}

void vfs_path_normalize(const char *in, char *out, u64 out_cap)
{
        char scratch[VFS_PATH_MAX];
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
                scratch[0] = '/';
                scratch[1] = '\0';
                vfs_path_collapse(scratch, out, out_cap);
                return;
        }

        scratch[0] = '/';
        i = 1;

        if (src[0] == '/') {
                src++;
        }

        while (*src != '\0' && i + 1 < sizeof(scratch)) {
                scratch[i++] = *src++;
        }

        scratch[i] = '\0';
        vfs_path_collapse(scratch, out, out_cap);
}

bool vfs_path_is_root(const char *path)
{
        char norm[VFS_PATH_MAX];

        if (!path) {
                return false;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        return vfs_path_is_root_norm(norm);
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
        if (vfs_path_is_root_norm(norm)) {
                return false;
        }

        len = strlen(norm);
        start = len;

        while (start > 1 && norm[start - 1] != '/') {
                start--;
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
        if (vfs_path_is_root_norm(norm)) {
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

static bool vfs_path_append_to_scratch(char *scratch, u64 scratch_cap,
                                       const char *suffix)
{
        u64 i = strlen(scratch);
        u64 j;

        if (!scratch || !suffix) {
                return false;
        }

        if (i > 0 && scratch[i - 1] != '/') {
                if (i + 1 >= scratch_cap) {
                        return false;
                }
                scratch[i++] = '/';
                scratch[i] = '\0';
        }

        for (j = 0; suffix[j] != '\0'; j++) {
                if (i + 1 >= scratch_cap) {
                        return false;
                }
                scratch[i++] = suffix[j];
        }

        scratch[i] = '\0';
        return true;
}

bool vfs_path_join(const char *base, const char *rel, char *out, u64 out_cap)
{
        char base_norm[VFS_PATH_MAX];
        char scratch[VFS_PATH_MAX];
        const char *r = rel;
        u64 i;
        u64 j;

        if (!base || !rel || !out || out_cap == 0) {
                return false;
        }

        if (rel[0] == '/') {
                vfs_path_normalize(rel, out, out_cap);
                return true;
        }

        vfs_path_normalize(base, base_norm, sizeof(base_norm));

        if (r[0] == '.' && r[1] == '/') {
                r += 2;
        } else if (r[0] == '.' && r[1] == '\0') {
                strncpy(out, base_norm, out_cap - 1);
                out[out_cap - 1] = '\0';
                return true;
        } else if (r[0] == '.' && r[1] == '.'
                   && (r[2] == '\0' || r[2] == '/')) {
                if (!vfs_path_parent(base_norm, scratch, sizeof(scratch))) {
                        strncpy(scratch, base_norm, sizeof(scratch) - 1);
                        scratch[sizeof(scratch) - 1] = '\0';
                }
                if (r[2] == '/') {
                        r += 3;
                        if (!vfs_path_append_to_scratch(scratch,
                                                        sizeof(scratch),
                                                        r)) {
                                return false;
                        }
                }
                vfs_path_normalize(scratch, out, out_cap);
                return true;
        }

        i = 0;
        for (j = 0; base_norm[j] != '\0' && i + 1 < sizeof(scratch); j++) {
                scratch[i++] = base_norm[j];
        }

        if (i == 0) {
                return false;
        }

        if (scratch[i - 1] != '/' && i + 1 < sizeof(scratch)) {
                scratch[i++] = '/';
        }

        for (j = 0; r[j] != '\0' && i + 1 < sizeof(scratch); j++) {
                scratch[i++] = r[j];
        }

        scratch[i] = '\0';
        vfs_path_normalize(scratch, out, out_cap);
        return true;
}
