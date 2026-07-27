#include "vfs_slice_table.h"

#include <common/mm.h>
#include <common/string.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>

static u64 vfs_st_byte_off(const vfs_slice_table_t *t, u32 index)
{
        u32 page_i = index / t->per_page;
        u32 slot = index % t->per_page;

        return (u64)page_i * (u64)PAGE_SIZE + (u64)slot * (u64)t->elem_size;
}

static u64 vfs_st_bytes_for_cap(const vfs_slice_table_t *t, u32 cap)
{
        u32 pages;

        if (cap == 0) {
                return 0;
        }
        pages = (cap + t->per_page - 1u) / t->per_page;
        return (u64)pages * (u64)PAGE_SIZE;
}

static error_t vfs_st_ensure_pgoff(struct page_slice *slice, u64 pgoff)
{
        struct allocator *alloc = percpu(kallocator);
        struct page_slice_entry *entry;
        vaddr page;

        if (!slice || !alloc) {
                return -E_IN_PARAM;
        }

        entry = page_slice_lookup(slice, pgoff);
        if (entry) {
                return REND_SUCCESS;
        }

        page = (vaddr)alloc->m_alloc(alloc, PAGE_SIZE);
        if (!page) {
                return -E_REND_NO_MEM;
        }

        memset((void *)page, 0, PAGE_SIZE);
        return page_slice_insert_page(slice, pgoff, page, 0);
}

error_t vfs_slice_table_init(vfs_slice_table_t *t, u32 elem_size,
                             u32 initial_cap)
{
        error_t err;
        u64 bytes;
        u64 pgoff;
        u64 pages;

        if (!t || elem_size == 0 || elem_size > PAGE_SIZE) {
                return -E_IN_PARAM;
        }

        memset(t, 0, sizeof(*t));
        t->elem_size = elem_size;
        t->per_page = (u32)(PAGE_SIZE / elem_size);
        t->soft_max = VFS_SLICE_TABLE_SOFT_MAX;
        if (t->per_page == 0) {
                return -E_IN_PARAM;
        }

        if (initial_cap == 0) {
                initial_cap = t->per_page;
        }
        if (initial_cap > t->soft_max) {
                return -E_IN_PARAM;
        }

        initial_cap = ((initial_cap + t->per_page - 1u) / t->per_page)
                      * t->per_page;
        bytes = vfs_st_bytes_for_cap(t, initial_cap);

        t->slice = page_slice_create(0, (size_t)bytes);
        if (!t->slice) {
                return -E_REND_NO_MEM;
        }

        pages = PAGE_SLICE_SIZE_TO_PAGE_COUNT(bytes);
        for (pgoff = 0; pgoff < pages; pgoff++) {
                err = vfs_st_ensure_pgoff(t->slice, pgoff);
                if (err != REND_SUCCESS) {
                        page_slice_destroy(&t->slice);
                        memset(t, 0, sizeof(*t));
                        return err;
                }
        }

        t->capacity = initial_cap;
        t->count = 0;
        return REND_SUCCESS;
}

void vfs_slice_table_destroy(vfs_slice_table_t *t)
{
        if (!t) {
                return;
        }
        if (t->slice) {
                page_slice_destroy(&t->slice);
        }
        memset(t, 0, sizeof(*t));
}

error_t vfs_slice_table_ensure_cap(vfs_slice_table_t *t, u32 need)
{
        u32 new_cap;
        u64 old_size;
        u64 new_size;
        u64 old_pages;
        u64 new_pages;
        u64 pgoff;
        error_t err;

        if (!t || !t->slice) {
                return -E_IN_PARAM;
        }
        if (need <= t->capacity) {
                return REND_SUCCESS;
        }
        if (need > t->soft_max) {
                return -E_REND_NO_MEM;
        }

        new_cap = t->capacity ? t->capacity : t->per_page;
        while (new_cap < need) {
                if (new_cap > t->soft_max / 2u) {
                        new_cap = t->soft_max;
                        break;
                }
                new_cap *= 2u;
        }
        new_cap = ((new_cap + t->per_page - 1u) / t->per_page) * t->per_page;
        if (new_cap > t->soft_max) {
                new_cap = (t->soft_max / t->per_page) * t->per_page;
        }
        if (new_cap < need) {
                return -E_REND_NO_MEM;
        }

        old_size = page_slice_get_size(t->slice);
        new_size = vfs_st_bytes_for_cap(t, new_cap);
        err = page_slice_set_size(&t->slice, new_size);
        if (err != REND_SUCCESS) {
                return err;
        }

        old_pages = PAGE_SLICE_SIZE_TO_PAGE_COUNT(old_size);
        new_pages = PAGE_SLICE_SIZE_TO_PAGE_COUNT(new_size);
        for (pgoff = old_pages; pgoff < new_pages; pgoff++) {
                err = vfs_st_ensure_pgoff(t->slice, pgoff);
                if (err != REND_SUCCESS) {
                        (void)page_slice_set_size(&t->slice, old_size);
                        return err;
                }
        }

        t->capacity = new_cap;
        return REND_SUCCESS;
}

void *vfs_slice_table_ptr(vfs_slice_table_t *t, u32 index)
{
        u64 off;
        struct page_slice_entry *entry;

        if (!t || !t->slice || index >= t->capacity) {
                return NULL;
        }

        off = vfs_st_byte_off(t, index);
        entry = page_slice_lookup(t->slice, PAGE_SLICE_BYTE_TO_PGOFF(off));
        if (!entry) {
                return NULL;
        }
        return (void *)(entry->kernel_virtual_address
                        + PAGE_SLICE_IN_PAGE_OFF(off));
}

error_t vfs_slice_table_push_zero(vfs_slice_table_t *t, u32 *idx_out)
{
        error_t err;
        void *p;
        u32 idx;

        if (!t || !idx_out) {
                return -E_IN_PARAM;
        }

        err = vfs_slice_table_ensure_cap(t, t->count + 1u);
        if (err != REND_SUCCESS) {
                return err;
        }

        idx = t->count;
        p = vfs_slice_table_ptr(t, idx);
        if (!p) {
                return -E_RENDEZVOS;
        }
        memset(p, 0, t->elem_size);
        t->count++;
        *idx_out = idx;
        return REND_SUCCESS;
}

void vfs_slice_table_pop_last(vfs_slice_table_t *t)
{
        void *p;

        if (!t || t->count == 0) {
                return;
        }
        p = vfs_slice_table_ptr(t, t->count - 1u);
        if (p) {
                memset(p, 0, t->elem_size);
        }
        t->count--;
}

bool vfs_dir_names_insert(vfs_slice_table_t *names, const char *name)
{
        u32 i;
        u32 count;
        u32 idx;
        char *p;
        error_t err;

        if (!names || names->elem_size != VFS_DIR_NAME_SLOT || !name
            || !name[0]) {
                return false;
        }

        count = vfs_slice_table_count(names);
        for (i = 0; i < count; i++) {
                p = (char *)vfs_slice_table_ptr(names, i);
                if (p && strcmp_s(p, name, VFS_DIR_NAME_SLOT) == 0) {
                        return true;
                }
        }

        err = vfs_slice_table_push_zero(names, &idx);
        if (err != REND_SUCCESS) {
                return false;
        }
        p = (char *)vfs_slice_table_ptr(names, idx);
        if (!p) {
                vfs_slice_table_pop_last(names);
                return false;
        }
        strncpy(p, name, VFS_DIR_NAME_SLOT - 1u);

        count = vfs_slice_table_count(names);
        for (i = count - 1u; i > 0; i--) {
                char *a = (char *)vfs_slice_table_ptr(names, i - 1u);
                char *b = (char *)vfs_slice_table_ptr(names, i);
                char tmp[VFS_DIR_NAME_SLOT];

                if (!a || !b) {
                        vfs_slice_table_pop_last(names);
                        return false;
                }
                if (strcmp_s(a, b, VFS_DIR_NAME_SLOT) <= 0) {
                        break;
                }
                memcpy(tmp, a, sizeof(tmp));
                memcpy(a, b, sizeof(tmp));
                memcpy(b, tmp, sizeof(tmp));
        }

        return true;
}
