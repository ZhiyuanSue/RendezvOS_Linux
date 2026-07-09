/*
 * ramfs — writable in-memory overlay for initramfs (Phase 4).
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
        char norm_a[VFS_PATH_MAX];
        char norm_b[VFS_PATH_MAX];

        vfs_path_normalize(a, norm_a, sizeof(norm_a));
        vfs_path_normalize(b, norm_b, sizeof(norm_b));
        return strcmp_s(norm_a, norm_b, VFS_PATH_MAX) == 0;
}

static void ramfs_free_entry_data(ramfs_entry_t *ent)
{
        struct allocator *alloc;

        if (!ent || !ent->data) {
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

        if (ent->flags & RAMFS_FLAG_WHITEOUT) {
                return -E_IN_PARAM;
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

const ramfs_entry_t *ramfs_entry_at(u32 index)
{
        if (index >= ramfs_nentries) {
                return NULL;
        }
        return &ramfs_entries[index];
}

const ramfs_entry_t *ramfs_lookup(const char *path)
{
        return ramfs_find_mutable(path);
}

bool ramfs_whiteout(const char *path)
{
        const ramfs_entry_t *ent = ramfs_lookup(path);

        return ent && (ent->flags & RAMFS_FLAG_WHITEOUT) != 0;
}

error_t ramfs_add_whiteout(const char *path)
{
        ramfs_entry_t *ent;
        error_t err;

        ent = ramfs_find_mutable(path);
        if (ent) {
                ramfs_free_entry_data(ent);
                ent->flags = RAMFS_FLAG_WHITEOUT;
                ent->mode = 0;
                ent->size = 0;
                return REND_SUCCESS;
        }

        err = ramfs_insert(path, 0, RAMFS_FLAG_WHITEOUT, &ent);
        return err;
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

i64 ramfs_read(const ramfs_entry_t *ent, u64 offset, void *buf, u64 len)
{
        u64 avail;

        if (!ent || !buf || (ent->flags & RAMFS_FLAG_WHITEOUT) != 0) {
                return -E_IN_PARAM;
        }

        if ((ent->flags & RAMFS_FLAG_DIR) != 0) {
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

        if (!ent || !buf || (ent->flags & RAMFS_FLAG_WHITEOUT) != 0) {
                return -E_IN_PARAM;
        }

        if ((ent->flags & RAMFS_FLAG_DIR) != 0) {
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
