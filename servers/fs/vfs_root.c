/*
 * Unified cpio (read-only) + ramfs (writable overlay) root for vfs_server.
 */

#include "cpio_rofs.h"
#include "vfs_backend_ops.h"
#include "vfs_kstat.h"
#include "vfs_root.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>

static bool vfs_root_path_is_dir(const char *path)
{
        vfs_inode_t ino;

        if (vfs_path_is_root(path)) {
                return true;
        }

        if (vfs_root_lookup(path, &ino) < 0) {
                return false;
        }

        return ino.is_dir;
}

static i64 vfs_root_err_from_rend(error_t err)
{
        switch (err) {
        case REND_SUCCESS:
                return 0;
        case -E_IN_PARAM:
                return -LINUX_EINVAL;
        default:
                return -LINUX_EIO;
        }
}

static i64 vfs_root_err_exists(void)
{
        return -LINUX_EEXIST;
}

static i64 vfs_root_err_noent(void)
{
        return -LINUX_ENOENT;
}

static i64 vfs_root_err_notdir(void)
{
        return -LINUX_ENOTDIR;
}

static i64 vfs_root_err_nomem(void)
{
        return -LINUX_ENOMEM;
}

static i64 vfs_root_check_parent_dir(const char *path)
{
        char parent[VFS_PATH_MAX];

        if (vfs_path_is_root(path)) {
                return vfs_root_err_noent();
        }

        if (!vfs_path_parent(path, parent, sizeof(parent))) {
                return -LINUX_EINVAL;
        }

        if (!vfs_root_path_is_dir(parent)) {
                return vfs_root_err_noent();
        }

        return 0;
}

static bool vfs_root_initialized;

error_t vfs_root_init(const void *cpio_image, u64 cpio_len)
{
        error_t err;

        if (vfs_root_initialized) {
                return REND_SUCCESS;
        }
        if (!cpio_image || cpio_len == 0) {
                return -E_IN_PARAM;
        }

        ramfs_init();
        err = cpio_rofs_init(cpio_image, cpio_len);
        if (err != REND_SUCCESS) {
                return err;
        }

        vfs_root_initialized = true;

        pr_info("[VFS] root ready: cpio %u entries, ramfs %u entries\n",
                cpio_rofs_parsed_count(),
                ramfs_entry_count());
        return REND_SUCCESS;
}

error_t vfs_root_ensure_init(const void *cpio_image, u64 cpio_len)
{
        return vfs_root_init(cpio_image, cpio_len);
}

i64 vfs_root_lookup(const char *path, vfs_inode_t *out)
{
        char norm[VFS_PATH_MAX];

        if (!path || !out) {
                return -LINUX_EINVAL;
        }

        memset(out, 0, sizeof(*out));
        vfs_path_normalize(path, norm, sizeof(norm));

        if (ramfs_whiteout(norm)) {
                return vfs_root_err_noent();
        }

        if (vfs_backend_ramfs_lookup(norm, out)) {
                return 0;
        }

        if (vfs_backend_cpio_lookup(norm, out)) {
                return 0;
        }

        if (vfs_path_is_root(norm)) {
                vfs_inode_init_synthetic_root(out);
                return 0;
        }

        return vfs_root_err_noent();
}

i64 vfs_root_mkdir(const char *path, u32 mode)
{
        char norm[VFS_PATH_MAX];
        i64 parent_err;
        error_t err;
        vfs_inode_t existing;

        if (!path) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));

        if (vfs_path_is_root(norm)) {
                return vfs_root_err_exists();
        }

        parent_err = vfs_root_check_parent_dir(norm);
        if (parent_err < 0) {
                return parent_err;
        }

        if (vfs_root_lookup(norm, &existing) == 0) {
                return vfs_root_err_exists();
        }

        err = ramfs_mkdir(norm, mode);
        if (err == -E_RENDEZVOS) {
                return vfs_root_err_exists();
        }
        if (err != REND_SUCCESS) {
                if (err == -E_IN_PARAM) {
                        return vfs_root_err_notdir();
                }
                return vfs_root_err_nomem();
        }

        return 0;
}

i64 vfs_root_create_file(const char *path, u32 mode, vfs_inode_t *out)
{
        char norm[VFS_PATH_MAX];
        i64 parent_err;
        error_t err;
        vfs_inode_t existing;

        if (!path) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));

        parent_err = vfs_root_check_parent_dir(norm);
        if (parent_err < 0) {
                return parent_err;
        }

        if (vfs_root_lookup(norm, &existing) == 0) {
                if (existing.is_dir) {
                        return -LINUX_EISDIR;
                }
                if (!existing.writable) {
                        return vfs_root_err_exists();
                }
                if (out) {
                        *out = existing;
                }
                return 0;
        }

        err = ramfs_create_file(norm, mode);
        if (err == -E_RENDEZVOS) {
                return vfs_root_err_exists();
        }
        if (err != REND_SUCCESS) {
                return vfs_root_err_nomem();
        }

        if (out && !vfs_backend_ramfs_lookup(norm, out)) {
                return -LINUX_EIO;
        }

        return 0;
}

i64 vfs_root_unlink(const char *path)
{
        char norm[VFS_PATH_MAX];
        vfs_inode_t ino;
        i64 look;
        error_t err;

        if (!path) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));

        if (vfs_path_is_root(norm)) {
                return -LINUX_EINVAL;
        }

        look = vfs_root_lookup(norm, &ino);
        if (look < 0) {
                return look;
        }

        if (ino.is_dir) {
                return -LINUX_EISDIR;
        }

        if (ino.backend == VFS_BACKEND_RAMFS && ino.u.ram) {
                err = ramfs_unlink(norm);
                return vfs_root_err_from_rend(err);
        }

        err = ramfs_add_whiteout(norm);
        if (err != REND_SUCCESS) {
                return vfs_root_err_nomem();
        }

        return 0;
}

i64 vfs_root_read(const vfs_inode_t *ino, u64 offset, void *buf, u64 len)
{
        return vfs_inode_read(ino, offset, buf, len);
}

i64 vfs_root_write(vfs_inode_t *ino, u64 offset, const void *buf, u64 len)
{
        return vfs_inode_write(ino, offset, buf, len);
}

i64 vfs_root_truncate(vfs_inode_t *ino, u64 size)
{
        return vfs_inode_truncate(ino, size);
}

i64 vfs_root_stat(const char *path, vfs_kstat_t *out)
{
        vfs_inode_t ino;
        i64 ret;

        if (!path || !out) {
                return -LINUX_EINVAL;
        }

        ret = vfs_root_lookup(path, &ino);
        if (ret < 0) {
                return ret;
        }

        vfs_kstat_from_inode(&ino, out);
        return 0;
}

#define VFS_READDIR_MAX_SLOTS 192

typedef struct vfs_readdir_slot {
        char name[VFS_PATH_MAX];
        u8 d_type;
        u64 d_ino;
} vfs_readdir_slot_t;

typedef struct vfs_readdir_build {
        char dir[VFS_PATH_MAX];
        vfs_readdir_slot_t slots[VFS_READDIR_MAX_SLOTS];
        u32 count;
} vfs_readdir_build_t;

/*
 * Single vfs_server thread only — do not put ~50KiB on the kernel stack
 * (192 slots × VFS_PATH_MAX names).
 */
static vfs_readdir_build_t vfs_readdir_scratch;

static i32 vfs_readdir_slot_cmp(const vfs_readdir_slot_t *a,
                                const vfs_readdir_slot_t *b)
{
        return (i32)strcmp_s(a->name, b->name, VFS_PATH_MAX);
}

static void vfs_readdir_sort(vfs_readdir_build_t *b)
{
        u32 i;
        u32 j;

        if (!b) {
                return;
        }

        for (i = 0; i + 1 < b->count; i++) {
                for (j = i + 1; j < b->count; j++) {
                        if (vfs_readdir_slot_cmp(&b->slots[i], &b->slots[j])
                            > 0) {
                                vfs_readdir_slot_t tmp = b->slots[i];

                                b->slots[i] = b->slots[j];
                                b->slots[j] = tmp;
                        }
                }
        }
}

static bool vfs_readdir_has_name(const vfs_readdir_build_t *b, const char *name)
{
        u32 i;

        if (!b || !name) {
                return false;
        }

        for (i = 0; i < b->count; i++) {
                if (strcmp_s(b->slots[i].name, name, VFS_PATH_MAX) == 0) {
                        return true;
                }
        }

        return false;
}

static bool vfs_readdir_add(vfs_readdir_build_t *b, const char *name, u8 d_type,
                            u64 d_ino)
{
        vfs_readdir_slot_t *slot;

        if (!b || !name || !name[0]) {
                return true;
        }

        if (vfs_readdir_has_name(b, name)) {
                return true;
        }

        if (b->count >= VFS_READDIR_MAX_SLOTS) {
                return false;
        }

        slot = &b->slots[b->count++];
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->name, name, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';
        slot->d_type = d_type;
        slot->d_ino = d_ino;
        return true;
}

static bool vfs_readdir_collect_ram(const ramfs_entry_t *ent, void *ctx)
{
        vfs_readdir_build_t *b = (vfs_readdir_build_t *)ctx;
        char name[VFS_PATH_MAX];
        u8 d_type;

        if (!ent || !b) {
                return true;
        }

        if (ent->flags & RAMFS_FLAG_WHITEOUT) {
                return true;
        }

        if (!vfs_path_direct_child_name(b->dir, ent->path, name, sizeof(name))) {
                return true;
        }

        d_type = (ent->flags & RAMFS_FLAG_DIR) ? VFS_DT_DIR : VFS_DT_REG;
        if (!vfs_readdir_add(b, name, d_type, vfs_path_to_ino(ent->path))) {
                return false;
        }

        return true;
}

static bool vfs_readdir_collect_cpio(const char *path,
                                     const cpio_rofs_stat_t *st, void *ctx)
{
        vfs_readdir_build_t *b = (vfs_readdir_build_t *)ctx;
        char name[VFS_PATH_MAX];
        u8 d_type;

        if (!path || !st || !b) {
                return true;
        }

        if (!vfs_path_direct_child_name(b->dir, path, name, sizeof(name))) {
                return true;
        }

        if (ramfs_whiteout(path) || ramfs_lookup(path)) {
                return true;
        }

        d_type = st->is_dir ? VFS_DT_DIR : VFS_DT_REG;
        if (!vfs_readdir_add(b, name, d_type, vfs_path_to_ino(path))) {
                return false;
        }

        return true;
}

static i64 vfs_readdir_build(vfs_readdir_build_t *b, const char *dirpath)
{
        vfs_inode_t dir;
        i64 look;
        u32 i;

        if (!b || !dirpath) {
                return -LINUX_EINVAL;
        }

        memset(b, 0, sizeof(*b));
        vfs_path_normalize(dirpath, b->dir, sizeof(b->dir));

        look = vfs_root_lookup(b->dir, &dir);
        if (look < 0) {
                return look;
        }
        if (!dir.is_dir) {
                return -LINUX_ENOTDIR;
        }

        for (i = 0; i < ramfs_entry_count(); i++) {
                const ramfs_entry_t *ent = ramfs_entry_at(i);

                if (!ent) {
                        continue;
                }
                if (!vfs_readdir_collect_ram(ent, b)) {
                        return -LINUX_EIO;
                }
        }

        cpio_rofs_visit(vfs_readdir_collect_cpio, b);
        vfs_readdir_sort(b);
        return 0;
}

i64 vfs_root_readdir(const char *dirpath, u64 index, vfs_dirent_t *out)
{
        i64 err;

        if (!dirpath || !out) {
                return -LINUX_EINVAL;
        }

        err = vfs_readdir_build(&vfs_readdir_scratch, dirpath);
        if (err < 0) {
                return err;
        }

        if (index >= vfs_readdir_scratch.count) {
                return 1;
        }

        memset(out, 0, sizeof(*out));
        strncpy(out->name,
                vfs_readdir_scratch.slots[index].name,
                sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
        out->d_type = vfs_readdir_scratch.slots[index].d_type;
        out->d_ino = vfs_readdir_scratch.slots[index].d_ino;
        return 0;
}
