#include "vfs_page_cache.h"

#include "cpio_rofs.h"
#include "ramfs_layer.h"
#include "vfs_backend.h"

#include <common/dsa/list.h>
#include <common/mm.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_path.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice_copy.h>
#include <rendezvos/smp/percpu.h>

#define VFS_PCACHE_MAX_ENTRIES 8u
#define VFS_PCACHE_MAX_CACHED  (256u * 1024u)

/* Upper-layer page_slice entry flag (not in core PAGE_SLICE_FLAG_*). */
#define VFS_PCACHE_PAGE_DIRTY (1ULL << 2)

typedef struct vfs_pcache_entry {
        char path[VFS_PATH_MAX];
        struct page_slice *slice;
        u64 size;
        u32 backend_caps;
        void *storage;
        bool dirty;
        bool valid;
        struct list_entry lru_node;
        struct list_entry pages_active;
        struct list_entry pages_inactive;
} vfs_pcache_entry_t;

static vfs_pcache_entry_t vfs_pcache[VFS_PCACHE_MAX_ENTRIES];
static LIST_HEAD(vfs_pcache_active);
static LIST_HEAD(vfs_pcache_inactive);

static struct allocator *vfs_pcache_alloc(void)
{
        return percpu(kallocator);
}

static vaddr vfs_inode_source_kva(const vfs_inode_t *ino)
{
        ramfs_entry_t *ram;

        if (!ino || !ino->storage) {
                return 0;
        }

        if (ino->backend_caps & VFS_BACKEND_CAP_WRITE_SOURCE) {
                ram = (ramfs_entry_t *)ino->storage;
                return ram->data ? (vaddr)ram->data : 0;
        }

        return (vaddr)ino->storage;
}

static ramfs_entry_t *vfs_inode_ramfs_storage(const vfs_inode_t *ino)
{
        if (!ino || !ino->storage
            || !(ino->backend_caps & VFS_BACKEND_CAP_WRITE_SOURCE)) {
                return NULL;
        }

        return (ramfs_entry_t *)ino->storage;
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

static void vfs_pcache_clear_page_lru(vfs_pcache_entry_t *ent)
{
        struct list_entry *pos;
        struct list_entry *n;

        if (!ent) {
                return;
        }

        if (list_node_is_valid(&ent->pages_active)
            && !list_empty(&ent->pages_active)) {
                list_for_each_safe(pos, n, &ent->pages_active) {
                        struct page_slice_entry *pe;

                        pe = list_entry(pos, struct page_slice_entry,
                                        page_list_node);
                        list_del_init(&pe->page_list_node);
                }
        }
        if (list_node_is_valid(&ent->pages_inactive)
            && !list_empty(&ent->pages_inactive)) {
                list_for_each_safe(pos, n, &ent->pages_inactive) {
                        struct page_slice_entry *pe;

                        pe = list_entry(pos, struct page_slice_entry,
                                        page_list_node);
                        list_del_init(&pe->page_list_node);
                }
        }
        INIT_LIST_HEAD(&ent->pages_active);
        INIT_LIST_HEAD(&ent->pages_inactive);
}

static void vfs_pcache_touch_file(vfs_pcache_entry_t *ent)
{
        if (!ent || !ent->valid) {
                return;
        }
        if (!list_node_is_detached(&ent->lru_node)) {
                list_del_init(&ent->lru_node);
        }
        list_add_tail(&ent->lru_node, &vfs_pcache_active);
}

static void vfs_pcache_page_link_inactive(vfs_pcache_entry_t *ent,
                                          struct page_slice_entry *pe)
{
        if (!ent || !pe) {
                return;
        }
        if (!list_node_is_detached(&pe->page_list_node)) {
                return;
        }
        list_add_tail(&pe->page_list_node, &ent->pages_inactive);
}

static void vfs_pcache_touch_page(vfs_pcache_entry_t *ent,
                                  struct page_slice_entry *pe)
{
        if (!ent || !pe) {
                return;
        }
        if (list_node_is_detached(&pe->page_list_node)) {
                list_add_tail(&pe->page_list_node, &ent->pages_inactive);
        }
        list_del_init(&pe->page_list_node);
        list_add_tail(&pe->page_list_node, &ent->pages_active);
}

static void vfs_pcache_touch_pages(vfs_pcache_entry_t *ent, u64 offset, u64 len)
{
        u64 done = 0;

        if (!ent || !ent->slice || len == 0) {
                return;
        }

        while (done < len) {
                u64 off = offset + done;
                u64 pgoff = PAGE_SLICE_BYTE_TO_PGOFF(off);
                u64 in_page = PAGE_SLICE_IN_PAGE_OFF(off);
                struct page_slice_entry *pe;
                u64 chunk;

                pe = page_slice_lookup(ent->slice, pgoff);
                if (!pe) {
                        break;
                }

                chunk = PAGE_SIZE - in_page;
                if (chunk > len - done) {
                        chunk = len - done;
                }

                vfs_pcache_touch_page(ent, pe);
                done += chunk;
        }
}

static void vfs_pcache_flush_entry(vfs_pcache_entry_t *ent)
{
        u8 *dst;

        if (!ent || !ent->valid || !ent->dirty || !ent->slice) {
                return;
        }

        if (ent->backend_caps & VFS_BACKEND_CAP_FLUSH_DROP) {
                ent->dirty = false;
                return;
        }

        if (!(ent->backend_caps & VFS_BACKEND_CAP_WRITE_SOURCE)
            || !ent->storage) {
                ent->dirty = false;
                return;
        }

        dst = ((ramfs_entry_t *)ent->storage)->data;
        if (!dst) {
                ent->dirty = false;
                return;
        }

        if (page_slice_copy_to_buffer(ent->slice, 0, dst, (size_t)ent->size)
            != REND_SUCCESS) {
                return;
        }

        ((ramfs_entry_t *)ent->storage)->size = ent->size;
        ent->dirty = false;
}

static void vfs_pcache_free_entry(vfs_pcache_entry_t *ent)
{
        if (!ent) {
                return;
        }

        vfs_pcache_flush_entry(ent);
        vfs_pcache_clear_page_lru(ent);
        if (list_node_is_valid(&ent->lru_node)
            && !list_node_is_detached(&ent->lru_node)) {
                list_del_init(&ent->lru_node);
        }
        if (ent->slice) {
                page_slice_destroy(&ent->slice);
        }

        ent->slice = NULL;
        ent->size = 0;
        ent->storage = NULL;
        ent->backend_caps = 0;
        ent->dirty = false;
        ent->valid = false;
        ent->path[0] = '\0';
        INIT_LIST_HEAD(&ent->lru_node);
}

static void vfs_pcache_evict_one(void)
{
        struct list_entry *node;
        vfs_pcache_entry_t *ent;

        if (!list_empty(&vfs_pcache_inactive)) {
                node = vfs_pcache_inactive.prev;
        } else if (!list_empty(&vfs_pcache_active)) {
                ent = list_entry(vfs_pcache_active.prev, vfs_pcache_entry_t,
                                 lru_node);
                list_del_init(&ent->lru_node);
                list_add_head(&ent->lru_node, &vfs_pcache_inactive);
                node = vfs_pcache_inactive.prev;
        } else {
                return;
        }

        ent = list_entry(node, vfs_pcache_entry_t, lru_node);
        vfs_pcache_free_entry(ent);
}

void vfs_page_cache_reset(void)
{
        u32 i;

        for (i = 0; i < VFS_PCACHE_MAX_ENTRIES; i++) {
                if (vfs_pcache[i].valid) {
                        vfs_pcache_free_entry(&vfs_pcache[i]);
                } else {
                        INIT_LIST_HEAD(&vfs_pcache[i].lru_node);
                        INIT_LIST_HEAD(&vfs_pcache[i].pages_active);
                        INIT_LIST_HEAD(&vfs_pcache[i].pages_inactive);
                }
        }
}

void vfs_page_cache_drop(const char *path)
{
        char norm[VFS_PATH_MAX];
        vfs_pcache_entry_t *ent;

        if (!path) {
                return;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        ent = vfs_pcache_find(norm);
        if (ent) {
                vfs_pcache_free_entry(ent);
        }
}

static error_t vfs_pcache_fill_slice_from_kva(struct page_slice **slice_out,
                                              vaddr src, u64 size,
                                              vfs_pcache_entry_t *ent)
{
        struct allocator *alloc = vfs_pcache_alloc();
        struct page_slice *slice;
        u64 pg_count;
        u64 pgoff;
        error_t err;

        if (!slice_out || src == 0 || size == 0 || !alloc) {
                return -E_IN_PARAM;
        }

        *slice_out = NULL;
        slice = page_slice_create(0, size);
        if (!slice) {
                return -E_REND_NO_MEM;
        }

        pg_count = PAGE_SLICE_SIZE_TO_PAGE_COUNT(size);
        for (pgoff = 0; pgoff < pg_count; pgoff++) {
                vaddr page = (vaddr)alloc->m_alloc(alloc, PAGE_SIZE);
                u64 off = pgoff * PAGE_SIZE;
                size_t copy_len = PAGE_SIZE;
                struct page_slice_entry *pe;

                if (!page) {
                        err = -E_REND_NO_MEM;
                        goto fail;
                }
                if (off + copy_len > size) {
                        copy_len = (size_t)(size - off);
                }

                memcpy((void *)page, (void *)(src + off), copy_len);
                if (copy_len < PAGE_SIZE) {
                        memset((void *)(page + copy_len), 0,
                               PAGE_SIZE - copy_len);
                }

                err = page_slice_insert_page(slice, pgoff, page, 0);
                if (err != REND_SUCCESS) {
                        alloc->m_free(alloc, (void *)page);
                        goto fail;
                }

                if (ent) {
                        pe = page_slice_lookup(slice, pgoff);
                        if (pe) {
                                INIT_LIST_HEAD(&pe->page_list_node);
                                vfs_pcache_page_link_inactive(ent, pe);
                        }
                }
        }

        *slice_out = slice;
        return REND_SUCCESS;
fail:
        page_slice_destroy(&slice);
        return err;
}

static error_t vfs_pcache_fill_from_inode(vfs_pcache_entry_t *ent,
                                          const vfs_inode_t *ino)
{
        if (!ent || !ino || ino->is_dir || ino->size == 0) {
                return -E_IN_PARAM;
        }

        vfs_pcache_clear_page_lru(ent);
        if (ent->slice) {
                page_slice_destroy(&ent->slice);
                ent->slice = NULL;
        }

        ent->size = ino->size;
        ent->backend_caps = ino->backend_caps;
        ent->storage = ino->storage;
        ent->dirty = false;

        if (vfs_inode_source_kva(ino) == 0) {
                return -E_IN_PARAM;
        }

        return vfs_pcache_fill_slice_from_kva(
                &ent->slice,
                vfs_inode_source_kva(ino),
                ino->size,
                ent);
}

static vfs_pcache_entry_t *vfs_pcache_alloc_slot(const char *path)
{
        u32 i;
        vfs_pcache_entry_t *ent;

        for (i = 0; i < VFS_PCACHE_MAX_ENTRIES; i++) {
                if (!vfs_pcache[i].valid) {
                        goto init_slot;
                }
        }

        vfs_pcache_evict_one();

        for (i = 0; i < VFS_PCACHE_MAX_ENTRIES; i++) {
                if (!vfs_pcache[i].valid) {
                        goto init_slot;
                }
        }

        return NULL;

init_slot:
        ent = &vfs_pcache[i];
        ent->slice = NULL;
        ent->size = 0;
        ent->storage = NULL;
        ent->backend_caps = 0;
        ent->dirty = false;
        INIT_LIST_HEAD(&ent->pages_active);
        INIT_LIST_HEAD(&ent->pages_inactive);
        INIT_LIST_HEAD(&ent->lru_node);
        strncpy(ent->path, path, sizeof(ent->path) - 1);
        ent->path[sizeof(ent->path) - 1] = '\0';
        ent->valid = true;
        list_add_tail(&ent->lru_node, &vfs_pcache_inactive);
        return ent;
}

static vfs_pcache_entry_t *vfs_pcache_ensure(const vfs_inode_t *ino)
{
        char norm[VFS_PATH_MAX];
        vfs_pcache_entry_t *ent;
        error_t err;

        if (!ino || ino->is_dir || ino->size == 0
            || ino->size > VFS_PCACHE_MAX_CACHED) {
                return NULL;
        }

        vfs_path_normalize(ino->path, norm, sizeof(norm));
        ent = vfs_pcache_find(norm);
        if (!ent) {
                ent = vfs_pcache_alloc_slot(norm);
                if (!ent) {
                        return NULL;
                }
                err = vfs_pcache_fill_from_inode(ent, ino);
                if (err != REND_SUCCESS) {
                        vfs_pcache_free_entry(ent);
                        return NULL;
                }
        }

        return ent;
}

static i64 vfs_pcache_read_direct(const vfs_inode_t *ino, u64 offset, void *buf,
                                  u64 len)
{
        cpio_rofs_stat_t st;
        i64 ret;

        if (!ino || !buf) {
                return -LINUX_EINVAL;
        }
        if (offset >= ino->size) {
                return 0;
        }

        if (len > ino->size - offset) {
                len = ino->size - offset;
        }

        if (ino->backend_caps & VFS_BACKEND_CAP_WRITE_SOURCE) {
                ramfs_entry_t *ram = vfs_inode_ramfs_storage(ino);

                if (!ram) {
                        return -LINUX_EINVAL;
                }
                ret = ramfs_read(ram, offset, buf, len);
        } else {
                st.mode = ino->mode;
                st.size = ino->size;
                st.is_dir = ino->is_dir;
                st.data = (const u8 *)ino->storage;
                ret = cpio_rofs_read(&st, offset, buf, len);
        }

        if (ret < 0) {
                return -LINUX_EINVAL;
        }
        return ret;
}

i64 vfs_page_cache_read_inode(const vfs_inode_t *ino, u64 offset, void *buf,
                              u64 len)
{
        vfs_pcache_entry_t *ent;
        u64 avail;
        error_t err;

        if (!ino || !buf) {
                return -LINUX_EINVAL;
        }
        if (ino->is_dir) {
                return -LINUX_EISDIR;
        }
        if (offset >= ino->size) {
                return 0;
        }

        if (len > ino->size - offset) {
                len = ino->size - offset;
        }

        ent = vfs_pcache_ensure(ino);
        if (!ent || !ent->slice) {
                return vfs_pcache_read_direct(ino, offset, buf, len);
        }

        vfs_pcache_touch_file(ent);

        avail = ent->size - offset;
        if (len > avail) {
                len = avail;
        }

        vfs_pcache_touch_pages(ent, offset, len);

        err = page_slice_copy_to_buffer(ent->slice, offset, buf, (size_t)len);
        if (err != REND_SUCCESS) {
                return vfs_pcache_read_direct(ino, offset, buf, len);
        }

        return (i64)len;
}

static error_t vfs_pcache_fill_owned_slice(const vfs_inode_t *ino,
                                           struct page_slice **out_slice)
{
        if (!ino || !out_slice || ino->is_dir || ino->size == 0) {
                return -E_IN_PARAM;
        }

        *out_slice = NULL;

        if (vfs_inode_source_kva(ino) == 0) {
                return -E_IN_PARAM;
        }

        return vfs_pcache_fill_slice_from_kva(
                out_slice,
                vfs_inode_source_kva(ino),
                ino->size,
                NULL);
}

i64 vfs_page_cache_clone_inode(const vfs_inode_t *ino,
                               struct page_slice **out_slice)
{
        vfs_pcache_entry_t *ent;
        error_t err;

        if (!ino || !out_slice) {
                return -LINUX_EINVAL;
        }
        if (ino->is_dir) {
                return -LINUX_EISDIR;
        }
        if (ino->size == 0) {
                return -LINUX_EINVAL;
        }

        *out_slice = NULL;

        if (ino->size > VFS_PCACHE_MAX_CACHED) {
                err = vfs_pcache_fill_owned_slice(ino, out_slice);
                return (err == REND_SUCCESS) ? 0 : -LINUX_ENOMEM;
        }

        ent = vfs_pcache_ensure(ino);
        if (!ent || !ent->slice) {
                err = vfs_pcache_fill_owned_slice(ino, out_slice);
                return (err == REND_SUCCESS) ? 0 : -LINUX_ENOMEM;
        }

        vfs_pcache_touch_file(ent);

        err = page_slice_clone(out_slice, ent->slice);
        if (err != REND_SUCCESS) {
                return -LINUX_ENOMEM;
        }

        return 0;
}

static i64 vfs_pcache_write_entry(vfs_pcache_entry_t *ent, u64 offset,
                                  const void *buf, u64 len)
{
        u64 pgoff;
        u64 done = 0;

        if (!ent || !ent->slice || !buf || len == 0) {
                return -LINUX_EINVAL;
        }

        vfs_pcache_touch_file(ent);

        if (offset >= ent->size) {
                return 0;
        }

        if (offset + len > ent->size) {
                len = ent->size - offset;
        }

        while (done < len) {
                u64 off = offset + done;
                u64 in_page = PAGE_SLICE_IN_PAGE_OFF(off);
                struct page_slice_entry *entry;
                size_t chunk;

                pgoff = PAGE_SLICE_BYTE_TO_PGOFF(off);
                entry = page_slice_lookup(ent->slice, pgoff);
                if (!entry) {
                        break;
                }

                chunk = PAGE_SIZE - (size_t)in_page;
                if (chunk > len - done) {
                        chunk = (size_t)(len - done);
                }

                memcpy((void *)(entry->kernel_virtual_address + in_page),
                       (const u8 *)buf + done,
                       chunk);
                entry->flags |= VFS_PCACHE_PAGE_DIRTY;
                vfs_pcache_touch_page(ent, entry);
                done += chunk;
        }

        ent->dirty = true;
        return (i64)done;
}

i64 vfs_page_cache_write_inode(const vfs_inode_t *ino, u64 offset,
                               const void *buf, u64 len)
{
        vfs_pcache_entry_t *ent;

        if (!ino || !buf) {
                return -LINUX_EINVAL;
        }
        if (ino->is_dir) {
                return -LINUX_EISDIR;
        }
        if (len == 0) {
                return 0;
        }

        ent = vfs_pcache_ensure(ino);
        if (!ent || !ent->slice) {
                return -LINUX_ENOMEM;
        }

        return vfs_pcache_write_entry(ent, offset, buf, len);
}

void vfs_page_cache_sync_write(const vfs_inode_t *ino, u64 offset,
                               const void *buf, u64 len)
{
        char norm[VFS_PATH_MAX];
        vfs_pcache_entry_t *ent;

        if (!ino || !buf || len == 0) {
                return;
        }

        vfs_path_normalize(ino->path, norm, sizeof(norm));
        ent = vfs_pcache_find(norm);
        if (!ent || !ent->slice) {
                return;
        }

        (void)vfs_pcache_write_entry(ent, offset, buf, len);
}

i64 vfs_page_cache_flush_backing(const vfs_inode_t *ino)
{
        char norm[VFS_PATH_MAX];
        vfs_pcache_entry_t *ent;

        if (!ino) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(ino->path, norm, sizeof(norm));
        ent = vfs_pcache_find(norm);
        if (!ent) {
                return 0;
        }

        if (ino->backend_caps & VFS_BACKEND_CAP_FLUSH_DROP) {
                vfs_pcache_free_entry(ent);
                return 0;
        }

        vfs_pcache_flush_entry(ent);
        return 0;
}

void vfs_page_cache_drop_backing(const vfs_inode_t *ino)
{
        if (!ino) {
                return;
        }
        vfs_page_cache_drop(ino->path);
}
