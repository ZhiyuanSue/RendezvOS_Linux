/*
 * ramfs — writable storage backend (kmalloc buffers).
 * Namespace/path state: vfs_namespace.c.
 */

#include "ramfs_layer.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>

#include "vfs_kstat.h"
#include "vfs_slice_table.h"

static vfs_slice_table_t ramfs_entry_tab;

static ramfs_entry_t *ramfs_entry_at(u32 i)
{
        return (ramfs_entry_t *)vfs_slice_table_ptr(&ramfs_entry_tab, i);
}

static struct allocator *ramfs_alloc(void)
{
        return percpu(kallocator);
}

static void ramfs_free_entry_data(ramfs_entry_t *ent)
{
        struct allocator *alloc;
        u32 refs;
        u32 i;
        u32 n;

        if (!ent || !ent->data) {
                return;
        }

        refs = 0;
        n = vfs_slice_table_count(&ramfs_entry_tab);
        for (i = 0; i < n; i++) {
                ramfs_entry_t *e = ramfs_entry_at(i);

                if (e && e->alive && e->data == ent->data) {
                        refs++;
                }
        }

        if (refs > 1) {
                ent->data = NULL;
                ent->size = 0;
                ent->capacity = 0;
                return;
        }

        alloc = ramfs_alloc();
        if (alloc) {
                alloc->m_free(alloc, ent->data);
        }
        ent->data = NULL;
        ent->size = 0;
        ent->capacity = 0;
}

static ramfs_entry_t *ramfs_find_mutable(const char *path)
{
        u32 i;
        u32 n;

        if (!path) {
                return NULL;
        }

        n = vfs_slice_table_count(&ramfs_entry_tab);
        for (i = 0; i < n; i++) {
                ramfs_entry_t *e = ramfs_entry_at(i);

                if (e && e->alive && vfs_path_equal(e->path, path)) {
                        return e;
                }
        }

        return NULL;
}

static ramfs_entry_t *ramfs_alloc_slot(u32 *idx_out)
{
        u32 i;
        u32 n;
        u32 idx;
        error_t err;
        ramfs_entry_t *e;

        n = vfs_slice_table_count(&ramfs_entry_tab);
        for (i = 0; i < n; i++) {
                e = ramfs_entry_at(i);

                if (e && !e->alive) {
                        memset(e, 0, sizeof(*e));
                        if (idx_out) {
                                *idx_out = i;
                        }
                        return e;
                }
        }

        err = vfs_slice_table_push_zero(&ramfs_entry_tab, &idx);
        if (err != REND_SUCCESS) {
                return NULL;
        }

        e = ramfs_entry_at(idx);
        if (!e) {
                vfs_slice_table_pop_last(&ramfs_entry_tab);
                return NULL;
        }
        if (idx_out) {
                *idx_out = idx;
        }
        return e;
}

static error_t ramfs_insert(const char *path, u32 mode, u8 flags,
                            ramfs_entry_t **out_ent)
{
        ramfs_entry_t *ent;

        if (!path || !out_ent) {
                return -E_IN_PARAM;
        }

        if (ramfs_find_mutable(path)) {
                return -E_RENDEZVOS;
        }

        ent = ramfs_alloc_slot(NULL);
        if (!ent) {
                return -E_REND_NO_MEM;
        }

        vfs_path_normalize(path, ent->path, sizeof(ent->path));
        ent->mode = mode;
        ent->flags = flags;
        ent->alive = true;
        *out_ent = ent;
        return REND_SUCCESS;
}

static error_t ramfs_ensure_parent_dir(const char *path)
{
        char parent[VFS_PATH_MAX];
        const ramfs_entry_t *ent;

        if (vfs_path_is_root(path)) {
                return -E_IN_PARAM;
        }

        if (!vfs_path_parent(path, parent, sizeof(parent))) {
                return -E_IN_PARAM;
        }

        if (vfs_path_is_root(parent)) {
                return REND_SUCCESS;
        }

        ent = ramfs_lookup(parent);
        if (!ent) {
                return REND_SUCCESS;
        }

        if (ent->flags & RAMFS_FLAG_DIR) {
                return REND_SUCCESS;
        }

        return -E_IN_PARAM;
}

static error_t ramfs_grow(ramfs_entry_t *ent, u64 need_size)
{
        struct allocator *alloc;
        u8 *new_data;
        u8 *old_data;
        u64 new_cap;
        u64 old_size;

        if (!ent || (ent->flags & RAMFS_FLAG_DIR) != 0) {
                return -E_IN_PARAM;
        }

        if (need_size > RAMFS_MAX_FILE_SIZE) {
                return -E_RENDEZVOS;
        }

        if (need_size <= ent->capacity) {
                return REND_SUCCESS;
        }

        new_cap = ent->capacity ? ent->capacity : 64;
        while (new_cap < need_size) {
                new_cap *= 2;
                if (new_cap > RAMFS_MAX_FILE_SIZE) {
                        new_cap = RAMFS_MAX_FILE_SIZE;
                }
        }

        alloc = ramfs_alloc();
        if (!alloc) {
                return -E_RENDEZVOS;
        }

        new_data = (u8 *)alloc->m_alloc(alloc, (size_t)new_cap);
        if (!new_data) {
                return -E_RENDEZVOS;
        }

        old_data = ent->data;
        old_size = ent->size;

        if (old_data && old_size > 0) {
                memcpy(new_data, old_data, (size_t)old_size);
        }

        if (old_data) {
                alloc->m_free(alloc, old_data);
        }

        ent->data = new_data;
        ent->capacity = new_cap;
        ent->size = old_size;
        return REND_SUCCESS;
}

void ramfs_init(void)
{
        error_t err;

        vfs_slice_table_destroy(&ramfs_entry_tab);
        err = vfs_slice_table_init(&ramfs_entry_tab, sizeof(ramfs_entry_t), 32);
        if (err != REND_SUCCESS) {
                pr_error("[VFS][ramfs] entry table init failed: %d\n", err);
        }
}

u32 ramfs_entry_count(void)
{
        return vfs_slice_table_count(&ramfs_entry_tab);
}

const ramfs_entry_t *ramfs_lookup(const char *path)
{
        return ramfs_find_mutable(path);
}

error_t ramfs_mkdir(const char *path, u32 mode)
{
        ramfs_entry_t *ent;
        error_t err;
        u32 dir_mode;

        err = ramfs_ensure_parent_dir(path);
        if (err != REND_SUCCESS) {
                return err;
        }

        if (ramfs_lookup(path)) {
                return -E_RENDEZVOS;
        }

        dir_mode = mode & (u32)~RAMFS_S_IFMT;
        dir_mode |= RAMFS_S_IFDIR;

        err = ramfs_insert(path, dir_mode, RAMFS_FLAG_DIR, &ent);
        if (err != REND_SUCCESS) {
                return err;
        }

        ent->size = 0;
        return REND_SUCCESS;
}

error_t ramfs_create_file(const char *path, u32 mode)
{
        ramfs_entry_t *ent;
        error_t err;
        u32 file_mode;

        err = ramfs_ensure_parent_dir(path);
        if (err != REND_SUCCESS) {
                return err;
        }

        if (ramfs_lookup(path)) {
                return -E_RENDEZVOS;
        }

        file_mode = mode & (u32)~RAMFS_S_IFMT;
        file_mode |= RAMFS_S_IFREG;

        err = ramfs_insert(path, file_mode, 0, &ent);
        if (err != REND_SUCCESS) {
                return err;
        }

        ent->size = 0;
        return REND_SUCCESS;
}

error_t ramfs_unlink(const char *path)
{
        ramfs_entry_t *ent;

        ent = ramfs_find_mutable(path);
        if (!ent) {
                return -E_IN_PARAM;
        }

        /*
         * Tombstone in place — do not swap-with-last. Open vfs_inode.storage
         * pointers must keep addressing the same slot for other live files.
         */
        ramfs_free_entry_data(ent);
        memset(ent, 0, sizeof(*ent));
        ent->alive = false;
        return REND_SUCCESS;
}

error_t ramfs_rename(const char *oldpath, const char *newpath)
{
        ramfs_entry_t *ent;
        char norm_old[VFS_PATH_MAX];
        char norm_new[VFS_PATH_MAX];
        error_t err;

        if (!oldpath || !newpath) {
                return -E_IN_PARAM;
        }

        vfs_path_normalize(oldpath, norm_old, sizeof(norm_old));
        vfs_path_normalize(newpath, norm_new, sizeof(norm_new));

        ent = ramfs_find_mutable(norm_old);
        if (!ent) {
                return -E_IN_PARAM;
        }

        if (ramfs_lookup(norm_new)) {
                return -E_RENDEZVOS;
        }

        err = ramfs_ensure_parent_dir(norm_new);
        if (err != REND_SUCCESS) {
                return err;
        }

        vfs_path_normalize(norm_new, ent->path, sizeof(ent->path));
        return REND_SUCCESS;
}

error_t ramfs_link(const char *oldpath, const char *newpath)
{
        ramfs_entry_t *src;
        ramfs_entry_t *dst;
        error_t err;

        if (!oldpath || !newpath) {
                return -E_IN_PARAM;
        }

        src = ramfs_find_mutable(oldpath);
        if (!src || (src->flags & RAMFS_FLAG_DIR) != 0) {
                return -E_IN_PARAM;
        }

        if (ramfs_lookup(newpath)) {
                return -E_RENDEZVOS;
        }

        err = ramfs_ensure_parent_dir(newpath);
        if (err != REND_SUCCESS) {
                return err;
        }

        err = ramfs_insert(newpath, src->mode, src->flags, &dst);
        if (err != REND_SUCCESS) {
                return err;
        }

        dst->data = src->data;
        dst->size = src->size;
        dst->capacity = src->capacity;
        return REND_SUCCESS;
}

i64 ramfs_read(const ramfs_entry_t *ent, u64 offset, void *buf, u64 len)
{
        u64 avail;

        if (!ent || !ent->alive || !buf || (ent->flags & RAMFS_FLAG_DIR) != 0) {
                return -E_IN_PARAM;
        }

        if (offset >= ent->size) {
                return 0;
        }

        avail = ent->size - offset;
        if (len > avail) {
                len = avail;
        }

        if (len > 0 && ent->data) {
                memcpy(buf, ent->data + offset, (size_t)len);
        }

        return (i64)len;
}

i64 ramfs_write(ramfs_entry_t *ent, u64 offset, const void *buf, u64 len)
{
        u64 end;
        error_t err;

        if (!ent || !ent->alive || !buf || (ent->flags & RAMFS_FLAG_DIR) != 0) {
                return -E_IN_PARAM;
        }

        if (len == 0) {
                return 0;
        }

        end = offset + len;
        if (end < offset || end > RAMFS_MAX_FILE_SIZE) {
                return -E_IN_PARAM;
        }

        err = ramfs_grow(ent, end);
        if (err != REND_SUCCESS) {
                return (i64)err;
        }

        memcpy(ent->data + offset, buf, (size_t)len);
        if (end > ent->size) {
                ent->size = end;
        }

        return (i64)len;
}

error_t ramfs_truncate(ramfs_entry_t *ent, u64 size)
{
        error_t err;

        if (!ent || !ent->alive || (ent->flags & RAMFS_FLAG_DIR) != 0) {
                return -E_IN_PARAM;
        }

        if (size > RAMFS_MAX_FILE_SIZE) {
                return -E_IN_PARAM;
        }

        if (size < ent->size) {
                ent->size = size;
                return REND_SUCCESS;
        }

        err = ramfs_grow(ent, size);
        if (err != REND_SUCCESS) {
                return err;
        }

        if (size > ent->size && ent->data) {
                memset(ent->data + ent->size, 0, (size_t)(size - ent->size));
        }

        ent->size = size;
        return REND_SUCCESS;
}

i64 ramfs_readdir(const char *dirpath, u64 index, vfs_dirent_t *out)
{
        char norm[VFS_PATH_MAX];
        char child_path[VFS_PATH_MAX];
        vfs_slice_table_t names;
        u32 name_count;
        u32 i;
        u32 n;
        char *picked;
        error_t err;

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

        err = vfs_slice_table_init(&names, VFS_DIR_NAME_SLOT, 16);
        if (err != REND_SUCCESS) {
                return -LINUX_ENOMEM;
        }

        n = vfs_slice_table_count(&ramfs_entry_tab);
        for (i = 0; i < n; i++) {
                ramfs_entry_t *ent = ramfs_entry_at(i);
                char child_name[VFS_DIR_NAME_SLOT];

                if (!ent || !ent->alive) {
                        continue;
                }
                if (!vfs_path_direct_child_name(
                            norm, ent->path, child_name, sizeof(child_name))) {
                        continue;
                }
                if (!vfs_dir_names_insert(&names, child_name)) {
                        vfs_slice_table_destroy(&names);
                        return -LINUX_ENOMEM;
                }
        }

        name_count = vfs_slice_table_count(&names);
        if (index >= name_count) {
                vfs_slice_table_destroy(&names);
                return 1;
        }

        picked = (char *)vfs_slice_table_ptr(&names, (u32)index);
        if (!picked
            || !vfs_path_join(norm, picked, child_path, sizeof(child_path))) {
                vfs_slice_table_destroy(&names);
                return -LINUX_EIO;
        }

        memset(out, 0, sizeof(*out));
        strncpy(out->name, picked, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';

        {
                const ramfs_entry_t *ent = ramfs_lookup(child_path);

                if (ent && (ent->flags & RAMFS_FLAG_DIR)) {
                        out->d_type = VFS_DT_DIR;
                } else if (ent) {
                        out->d_type = VFS_DT_REG;
                } else {
                        out->d_type = VFS_DT_UNKNOWN;
                }
        }

        out->d_ino = vfs_path_to_ino(child_path);
        vfs_slice_table_destroy(&names);
        return 0;
}
