#include <linux_compat/mm/linux_page_slice_file.h>

#include <common/mm.h>
#include <common/string.h>
#include <rendezvos/mm/page_slice_copy.h>

error_t linux_page_slice_copy_from_kva(struct page_slice** slice_out,
                                       struct allocator* alloc, vaddr src,
                                       size_t size)
{
        struct page_slice* slice;
        u64 pg_count;
        u64 pgoff;
        error_t err;

        if (!slice_out || !alloc || src == 0 || size == 0)
                return -E_IN_PARAM;

        *slice_out = NULL;
        slice = page_slice_create(0, size);
        if (!slice)
                return -E_RENDEZVOS;

        pg_count = PAGE_SLICE_SIZE_TO_PAGE_COUNT(size);
        for (pgoff = 0; pgoff < pg_count; pgoff++) {
                vaddr page = (vaddr)alloc->m_alloc(alloc, PAGE_SIZE);
                u64 off = pgoff * PAGE_SIZE;
                size_t copy_len = PAGE_SIZE;

                if (!page) {
                        err = -E_RENDEZVOS;
                        goto fail;
                }
                if (off + copy_len > size)
                        copy_len = (size_t)(size - off);

                memcpy((void*)page, (void*)(src + off), copy_len);
                if (copy_len < PAGE_SIZE)
                        memset((void*)(page + copy_len),
                               0,
                               PAGE_SIZE - copy_len);

                err = page_slice_insert_page(slice, pgoff, page, 0);
                if (err != REND_SUCCESS) {
                        alloc->m_free(alloc, (void*)page);
                        goto fail;
                }
        }

        *slice_out = slice;
        return REND_SUCCESS;

fail:
        page_slice_destroy(&slice);
        return err;
}

vaddr linux_page_slice_file_base(struct page_slice* slice)
{
        struct page_slice_entry* entry;

        if (!slice) {
                return 0;
        }

        entry = page_slice_lookup(slice, 0);
        if (!entry) {
                return 0;
        }

        return entry->kernel_virtual_address;
}
