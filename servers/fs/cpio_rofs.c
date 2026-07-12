/*
 * newc cpio (070701) read-only parser for embedded initramfs.
 */

#include "cpio_rofs.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>

#include "vfs_kstat.h"

#define CPIO_NEWC_MAGIC       "070701"
#define CPIO_NEWC_HDR_LEN     110
#define CPIO_NEWC_MAX_ENTRIES 64

#define CPIO_S_IFMT  0170000u
#define CPIO_S_IFDIR 0040000u
#define CPIO_S_IFLNK 0120000u

typedef struct cpio_rofs_entry {
        char path[CPIO_ROFS_PATH_MAX];
        u32 mode;
        u32 nlink;
        u64 filesize;
        const u8 *data;
        bool is_dir;
        bool is_symlink;
} cpio_rofs_entry_t;

typedef struct cpio_newc_header {
        char magic[6];
        char ino[8];
        char mode[8];
        char uid[8];
        char gid[8];
        char nlink[8];
        char mtime[8];
        char filesize[8];
        char devmajor[8];
        char devminor[8];
        char rdevmajor[8];
        char rdevminor[8];
        char namesize[8];
        char check[8];
} cpio_newc_header_t;

static bool cpio_header_magic_ok(const cpio_newc_header_t *hdr)
{
        static const char magic[] = CPIO_NEWC_MAGIC;

        if (!hdr) {
                return false;
        }

        for (u32 i = 0; i < 6; i++) {
                if (hdr->magic[i] != magic[i]) {
                        return false;
                }
        }

        return true;
}

static cpio_rofs_entry_t cpio_entries[CPIO_NEWC_MAX_ENTRIES];
static u32 cpio_entry_count;
static const u8 *cpio_image;
static u64 cpio_image_len;

static u64 cpio_hex_field8(const char *field)
{
        u64 value = 0;
        u32 i;

        for (i = 0; i < 8; i++) {
                char c = field[i];

                value <<= 4;
                if (c >= '0' && c <= '9') {
                        value |= (u64)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                        value |= (u64)(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                        value |= (u64)(c - 'A' + 10);
                }
        }

        return value;
}

static u64 cpio_align4(u64 value)
{
        return (value + 3) & ~((u64)3);
}

/* newc: pathname is padded so (header + name) is 4-byte aligned, not namesize
 * alone. */
static u64 cpio_name_field_len(u64 namesize)
{
        return cpio_align4(CPIO_NEWC_HDR_LEN + namesize) - CPIO_NEWC_HDR_LEN;
}

static bool cpio_name_skip(const char *name)
{
        if (!name || !name[0]) {
                return true;
        }
        if (strcmp_s(name, ".", 2) == 0) {
                return true;
        }
        if (strcmp_s(name, "..", 3) == 0) {
                return true;
        }
        if (strcmp_s(name, "TRAILER!!!", 11) == 0) {
                return true;
        }
        return false;
}

static void cpio_normalize_path(const char *name, char *out, u64 out_cap)
{
        const char *src = name;
        u64 i = 0;

        if (!out || out_cap == 0) {
                return;
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

static error_t cpio_rofs_add_entry(const char *name, u32 mode, u32 nlink,
                                   u64 filesize, const u8 *data, bool is_dir,
                                   bool is_symlink)
{
        cpio_rofs_entry_t *ent;

        if (cpio_name_skip(name)) {
                return REND_SUCCESS;
        }

        if (cpio_entry_count >= CPIO_NEWC_MAX_ENTRIES) {
                pr_error("[VFS][cpio] entry table full (max %u)\n",
                         CPIO_NEWC_MAX_ENTRIES);
                return -E_RENDEZVOS;
        }

        ent = &cpio_entries[cpio_entry_count];
        memset(ent, 0, sizeof(*ent));
        cpio_normalize_path(name, ent->path, sizeof(ent->path));
        ent->mode = mode;
        ent->nlink = nlink;
        ent->filesize = filesize;
        ent->data = data;
        ent->is_dir = is_dir;
        ent->is_symlink = is_symlink;

        cpio_entry_count++;
        return REND_SUCCESS;
}

error_t cpio_rofs_init(const void *image, u64 image_len)
{
        const u8 *cursor;
        const u8 *end;
        error_t err;

        cpio_entry_count = 0;
        cpio_image = (const u8 *)image;
        cpio_image_len = image_len;

        if (!image || image_len < CPIO_NEWC_HDR_LEN) {
                pr_error("[VFS][cpio] image too small (%llu bytes)\n",
                         (u64)image_len);
                return -E_IN_PARAM;
        }

        cursor = (const u8 *)image;
        end = cursor + image_len;

        while (cursor + CPIO_NEWC_HDR_LEN <= end) {
                const cpio_newc_header_t *hdr =
                        (const cpio_newc_header_t *)cursor;
                u64 namesize;
                u64 filesize;
                u64 mode;
                u64 nlink;
                const char *name;
                const u8 *data;
                bool is_dir;
                bool is_symlink;

                if (!cpio_header_magic_ok(hdr)) {
                        pr_error("[VFS][cpio] bad magic at offset %llu\n",
                                 (u64)(cursor - (const u8 *)image));
                        return -E_IN_PARAM;
                }

                namesize = cpio_hex_field8(hdr->namesize);
                filesize = cpio_hex_field8(hdr->filesize);
                mode = cpio_hex_field8(hdr->mode);
                nlink = (u32)cpio_hex_field8(hdr->nlink);

                cursor += CPIO_NEWC_HDR_LEN;

                if (cursor + namesize > end) {
                        pr_error("[VFS][cpio] truncated name at %llu\n",
                                 (u64)(cursor - (const u8 *)image));
                        return -E_IN_PARAM;
                }

                name = (const char *)cursor;
                if (strcmp_s(name, "TRAILER!!!", 11) == 0) {
                        break;
                }

                cursor += cpio_name_field_len(namesize);

                data = cursor;
                if (cursor + filesize > end) {
                        pr_error("[VFS][cpio] truncated file %s\n", name);
                        return -E_IN_PARAM;
                }

                is_symlink = ((mode & CPIO_S_IFMT) == CPIO_S_IFLNK);
                is_dir = ((mode & CPIO_S_IFMT) == CPIO_S_IFDIR);

                err = cpio_rofs_add_entry(
                        name, (u32)mode, nlink, filesize, data, is_dir,
                        is_symlink);
                if (err != REND_SUCCESS) {
                        return err;
                }

                cursor += cpio_align4(filesize);
        }

        return REND_SUCCESS;
}

u32 cpio_rofs_parsed_count(void)
{
        return cpio_entry_count;
}

static bool cpio_path_equal(const char *a, const char *b)
{
        char norm_a[CPIO_ROFS_PATH_MAX];
        char norm_b[CPIO_ROFS_PATH_MAX];

        cpio_normalize_path(a, norm_a, sizeof(norm_a));
        cpio_normalize_path(b, norm_b, sizeof(norm_b));
        return strcmp_s(norm_a, norm_b, CPIO_ROFS_PATH_MAX) == 0;
}

bool cpio_rofs_lookup(const char *path, cpio_rofs_stat_t *out)
{
        u32 i;

        if (!path || !out) {
                return false;
        }

        memset(out, 0, sizeof(*out));

        for (i = 0; i < cpio_entry_count; i++) {
                if (cpio_path_equal(cpio_entries[i].path, path)) {
                        out->mode = cpio_entries[i].mode;
                        out->size = cpio_entries[i].filesize;
                        out->is_dir = cpio_entries[i].is_dir;
                        out->is_symlink = cpio_entries[i].is_symlink;
                        out->data = cpio_entries[i].data;
                        out->nlink = cpio_entries[i].nlink ?
                                             cpio_entries[i].nlink :
                                             1u;
                        return true;
                }
        }

        return false;
}

bool cpio_rofs_ptr_in_image(const void *ptr)
{
        const u8 *p;

        if (!cpio_image || !ptr) {
                return false;
        }

        p = (const u8 *)ptr;
        return p >= cpio_image && p < cpio_image + cpio_image_len;
}

i64 cpio_rofs_read(const cpio_rofs_stat_t *st, u64 offset, void *buf, u64 len)
{
        u64 avail;

        if (!st || st->is_dir || !buf) {
                return -E_IN_PARAM;
        }

        if (offset >= st->size) {
                return 0;
        }

        avail = st->size - offset;
        if (len > avail) {
                len = avail;
        }

        if (len > 0 && st->data) {
                memcpy(buf, st->data + offset, (size_t)len);
        }

        return (i64)len;
}

void cpio_rofs_visit(cpio_rofs_visit_fn fn, void *ctx)
{
        u32 i;

        if (!fn) {
                return;
        }

        for (i = 0; i < cpio_entry_count; i++) {
                cpio_rofs_stat_t st;

                memset(&st, 0, sizeof(st));
                st.mode = cpio_entries[i].mode;
                st.size = cpio_entries[i].filesize;
                st.is_dir = cpio_entries[i].is_dir;
                st.is_symlink = cpio_entries[i].is_symlink;
                st.data = cpio_entries[i].data;
                st.nlink = cpio_entries[i].nlink ? cpio_entries[i].nlink : 1u;

                if (!fn(cpio_entries[i].path, &st, ctx)) {
                        break;
                }
        }
}

static i32 cpio_readdir_name_cmp(const char *a, const char *b)
{
        return (i32)strcmp_s(a, b, 64);
}

static bool cpio_readdir_insert_name(char names[][64], u32 *count, const char *name)
{
        u32 i;

        if (!names || !count || !name || !name[0]) {
                return false;
        }

        for (i = 0; i < *count; i++) {
                if (cpio_readdir_name_cmp(names[i], name) == 0) {
                        return true;
                }
        }

        if (*count >= CPIO_NEWC_MAX_ENTRIES) {
                return false;
        }

        strncpy(names[*count], name, 63);
        names[*count][63] = '\0';

        for (i = *count; i > 0; i--) {
                if (cpio_readdir_name_cmp(names[i - 1], names[i]) <= 0) {
                        break;
                }
                {
                        char tmp[64];

                        strncpy(tmp, names[i - 1], sizeof(tmp) - 1);
                        tmp[sizeof(tmp) - 1] = '\0';
                        strncpy(names[i - 1], names[i], 63);
                        names[i - 1][63] = '\0';
                        strncpy(names[i], tmp, 63);
                        names[i][63] = '\0';
                }
        }

        (*count)++;
        return true;
}

static u8 cpio_readdir_dtype(const char *child_path)
{
        cpio_rofs_stat_t st;

        if (!cpio_rofs_lookup(child_path, &st)) {
                return VFS_DT_UNKNOWN;
        }
        if (st.is_symlink) {
                return VFS_DT_LNK;
        }
        if (st.is_dir) {
                return VFS_DT_DIR;
        }
        return VFS_DT_REG;
}

i64 cpio_rofs_readdir(const char *dirpath, u64 index, vfs_dirent_t *out)
{
        char norm[VFS_PATH_MAX];
        char child_path[VFS_PATH_MAX];
        char names[CPIO_NEWC_MAX_ENTRIES][64];
        u32 name_count = 0;
        u32 i;

        if (!dirpath || !out) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(dirpath, norm, sizeof(norm));

        if (index == 0) {
                memset(out, 0, sizeof(*out));
                strncpy(out->name, ".", sizeof(out->name) - 1);
                out->d_type = VFS_DT_DIR;
                out->d_ino = vfs_path_to_ino(norm);
                return 0;
        }
        if (index == 1) {
                char parent[VFS_PATH_MAX];

                memset(out, 0, sizeof(*out));
                strncpy(out->name, "..", sizeof(out->name) - 1);
                out->d_type = VFS_DT_DIR;
                if (vfs_path_parent(norm, parent, sizeof(parent))) {
                        out->d_ino = vfs_path_to_ino(parent);
                } else {
                        out->d_ino = vfs_path_to_ino("/");
                }
                return 0;
        }

        index -= 2;

        for (i = 0; i < cpio_entry_count; i++) {
                char child_name[64];

                if (!vfs_path_direct_child_name(
                            norm, cpio_entries[i].path, child_name,
                            sizeof(child_name))) {
                        continue;
                }
                (void)cpio_readdir_insert_name(names, &name_count, child_name);
        }

        if (index >= name_count) {
                return 1;
        }

        if (!vfs_path_join(norm, names[index], child_path, sizeof(child_path))) {
                return -LINUX_EIO;
        }

        memset(out, 0, sizeof(*out));
        strncpy(out->name, names[index], sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
        out->d_type = cpio_readdir_dtype(child_path);
        out->d_ino = vfs_path_to_ino(child_path);
        return 0;
}
