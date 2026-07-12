#include "vfs_page_cache.h"

#include <linux_compat/fs/vfs_path.h>

#include <common/string.h>
#include <linux_compat/errno.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>

#define VFS_PCACHE_MAX_ENTRIES 8u
#define VFS_PCACHE_MAX_FILE    (256u * 1024u)

typedef struct vfs_pcache_entry {
        char path[VFS_PATH_MAX];
        u8 *data;
        u64 size;
        bool valid;
} vfs_pcache_entry_t;

static vfs_pcache_entry_t vfs_pcache[VFS_PCACHE_MAX_ENTRIES];

static struct allocator *vfs_pcache_alloc(void)
{
        return percpu(kallocator);
}

static vfs_pcache_entry_t *vfs_pcache_find(const char *path)
{
        u32 i;

        if (!path) {
                return NULL;
        }

        for (i = 0; i < VFS_PCACHE_MAX_ENTRIES; i++) {
                if (!vfs_pcache[i].valid) {
                        continue;
                }
                if (strcmp_s(vfs_pcache[i].path, path, VFS_PATH_MAX) == 0) {
                        return &vfs_pcache[i];
                }
        }

        return NULL;
}

static void vfs_pcache_free_entry(vfs_pcache_entry_t *ent)
{
        struct allocator *alloc;

        if (!ent) {
                return;
        }

        alloc = vfs_pcache_alloc();
        if (ent->data && alloc) {
                alloc->m_free(alloc, ent->data);
        }

        ent->data = NULL;
        ent->size = 0;
        ent->valid = false;
        ent->path[0] = '\0';
}

void vfs_page_cache_reset(void)
{
        u32 i;

        for (i = 0; i < VFS_PCACHE_MAX_ENTRIES; i++) {
                vfs_pcache_free_entry(&vfs_pcache[i]);
        }
}

void vfs_page_cache_drop(const char *path)
{
        vfs_pcache_entry_t *ent = vfs_pcache_find(path);

        if (ent) {
                vfs_pcache_free_entry(ent);
        }
}

static vfs_pcache_entry_t *vfs_pcache_alloc_slot(const char *path)
{
        u32 i;
        u32 victim = 0;

        ent_found:
        for (i = 0; i < VFS_PCACHE_MAX_ENTRIES; i++) {
                if (!vfs_pcache[i].valid) {
                        strncpy(vfs_pcache[i].path, path,
                                sizeof(vfs_pcache[i].path) - 1);
                        vfs_pcache[i].path[sizeof(vfs_pcache[i].path) - 1] = '\0';
                        vfs_pcache[i].valid = true;
                        return &vfs_pcache[i];
                }
        }

        vfs_pcache_free_entry(&vfs_pcache[victim]);
        goto ent_found;
}

static i64 vfs_pcache_fill(vfs_pcache_entry_t *ent, const char *path,
                           const u8 *data, u64 size)
{
        struct allocator *alloc;
        u8 *copy;

        if (!ent || !path || !data || size > VFS_PCACHE_MAX_FILE) {
                return -LINUX_EINVAL;
        }

        alloc = vfs_pcache_alloc();
        if (!alloc) {
                return -LINUX_ENOMEM;
        }

        copy = (u8 *)alloc->m_alloc(alloc, (size_t)size);
        if (!copy) {
                return -LINUX_ENOMEM;
        }

        memcpy(copy, data, (size_t)size);
        ent->data = copy;
        ent->size = size;
        strncpy(ent->path, path, sizeof(ent->path) - 1);
        ent->path[sizeof(ent->path) - 1] = '\0';
        ent->valid = true;
        return 0;
}

i64 vfs_page_cache_read_cpio(const char *path, const u8 *data, u64 size,
                             u64 offset, void *buf, u64 len)
{
        char norm[VFS_PATH_MAX];
        vfs_pcache_entry_t *ent;
        u64 avail;
        i64 err;

        if (!path || !buf) {
                return -LINUX_EINVAL;
        }
        if (!data && size > 0) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        if (offset >= size) {
                return 0;
        }

        ent = vfs_pcache_find(norm);
        if (!ent) {
                ent = vfs_pcache_alloc_slot(norm);
                if (!ent) {
                        return -LINUX_ENOMEM;
                }
                err = vfs_pcache_fill(ent, norm, data, size);
                if (err < 0) {
                        vfs_pcache_free_entry(ent);
                        if (len == 0) {
                                return 0;
                        }
                        avail = size - offset;
                        if (len > avail) {
                                len = avail;
                        }
                        memcpy(buf, data + offset, (size_t)len);
                        return (i64)len;
                }
        }

        avail = ent->size - offset;
        if (len > avail) {
                len = avail;
        }
        if (len > 0) {
                memcpy(buf, ent->data + offset, (size_t)len);
        }
        return (i64)len;
}
