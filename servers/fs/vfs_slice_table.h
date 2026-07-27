#ifndef _VFS_SLICE_TABLE_H_
#define _VFS_SLICE_TABLE_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/page_slice.h>

/*
 * Growable record table on page_slice.
 *
 * Records are packed so none straddle PAGE_SIZE: each page holds
 * per_page = PAGE_SIZE / elem_size elements. Growth only inserts new pages;
 * existing KVAs stay valid (safe for in-table pointers, e.g. namespace links).
 *
 * Soft max (VFS_SLICE_TABLE_SOFT_MAX) caps runaway growth — not a BSS size.
 */

#ifndef VFS_SLICE_TABLE_SOFT_MAX
#define VFS_SLICE_TABLE_SOFT_MAX (1u << 20)
#endif

typedef struct vfs_slice_table {
        struct page_slice *slice;
        u32 elem_size;
        u32 per_page;
        u32 count;
        u32 capacity;
        u32 soft_max;
} vfs_slice_table_t;

error_t vfs_slice_table_init(vfs_slice_table_t *t, u32 elem_size,
                             u32 initial_cap);
void vfs_slice_table_destroy(vfs_slice_table_t *t);

error_t vfs_slice_table_ensure_cap(vfs_slice_table_t *t, u32 need);

/* In-slice pointer for @index (< capacity). NULL if unmapped / OOB. */
void *vfs_slice_table_ptr(vfs_slice_table_t *t, u32 index);

/* Append a zeroed slot; writes index to @idx_out. */
error_t vfs_slice_table_push_zero(vfs_slice_table_t *t, u32 *idx_out);

/* Drop the last slot (count--). Does not shrink capacity. */
void vfs_slice_table_pop_last(vfs_slice_table_t *t);

static inline u32 vfs_slice_table_count(const vfs_slice_table_t *t)
{
        return t ? t->count : 0;
}

/*
 * Sorted unique dirent-name helper (elem_size must be 64).
 * Used by cpio/ramfs readdir scratch tables.
 */
#define VFS_DIR_NAME_SLOT 64u

bool vfs_dir_names_insert(vfs_slice_table_t *names, const char *name);

#endif /* _VFS_SLICE_TABLE_H_ */
