/*
 * ramfs — writable storage backend (kmalloc buffers).
 * Namespace/path state: vfs_namespace.c.
 */

#include "ramfs_layer.h"

#include <common/string.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>

static ramfs_entry_t ramfs_entries[RAMFS_MAX_ENTRIES];
static u32 ramfs_nentries;

static struct allocator *ramfs_alloc(void)
{
        return percpu(kallocator);
}

static bool ramfs_path_equal(const char *a, const char *b)
{
        return vfs_path_equal(a, b);
}

static void ramfs_free_entry_data(ramfs_entry_t *ent)
{
        struct allocator *alloc;
        u32 refs;
        u32 i;

        if (!ent || !ent->data) {
                return;
        }

        refs = 0;
        for (i = 0; i < ramfs_nentries; i++) {
                if (ramfs_entries[i].data == ent->data) {
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

        if (!path) {
                return NULL;
        }

        for (i = 0; i < ramfs_nentries; i++) {
                if (ramfs_path_equal(ramfs_entries[i].path, path)) {
                        return &ramfs_entries[i];
                }
        }

        return NULL;
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

        if (ramfs_nentries >= RAMFS_MAX_ENTRIES) {
                pr_error("[VFS][ramfs] entry table full (max %u)\n",
                         RAMFS_MAX_ENTRIES);
                return -E_RENDEZVOS;
        }

        ent = &ramfs_entries[ramfs_nentries++];
        memset(ent, 0, sizeof(*ent));
        vfs_path_normalize(path, ent->path, sizeof(ent->path));
        ent->mode = mode;
        ent->flags = flags;
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
        ramfs_nentries = 0;
        memset(ramfs_entries, 0, sizeof(ramfs_entries));
}

u32 ramfs_entry_count(void)
{
        return ramfs_nentries;
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
        u32 i;

        ent = ramfs_find_mutable(path);
        if (!ent) {
                return -E_IN_PARAM;
        }

        ramfs_free_entry_data(ent);

        for (i = 0; i < ramfs_nentries; i++) {
                if (&ramfs_entries[i] == ent) {
                        break;
                }
        }

        if (i + 1 < ramfs_nentries) {
                ramfs_entries[i] = ramfs_entries[ramfs_nentries - 1];
        }
        ramfs_nentries--;
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

        if (!ent || !buf || (ent->flags & RAMFS_FLAG_DIR) != 0) {
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

        if (!ent || !buf || (ent->flags & RAMFS_FLAG_DIR) != 0) {
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

        if (!ent || (ent->flags & RAMFS_FLAG_DIR) != 0) {
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
