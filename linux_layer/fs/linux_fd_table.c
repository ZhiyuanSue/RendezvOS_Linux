/*
 * Per-process fd table (linux_layer) — page_slice-backed, dynamically growable.
 * See doc/linux_compat/FD_TABLE.md.
 */

#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/linux_pipe.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/errno.h>

#include <common/mm.h>
#include <common/string.h>
#include <rendezvos/error.h>
#include <common/stddef.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice_copy.h>
#include <rendezvos/smp/percpu.h>

#define LINUX_FS_FD_REGION_BASE (2ULL * PAGE_SIZE)
#define LINUX_FS_SLICE_FD_GROW  64u

/*
 * Slice header at byte offset 0: process-wide FS metadata (cwd + capacity).
 * Per-fd state (kind, handle, vfs_abs_path, is_dir) lives at LINUX_FS_FD_REGION_BASE.
 * Never allocate this struct on the kernel stack — kstack is only 8 KiB.
 */
typedef struct linux_fs_slice_hdr {
        char cwd[LINUX_VFS_PATH_MAX];
        u32 fd_capacity;
} linux_fs_slice_hdr_t;

#define LINUX_FS_HDR_FD_CAPACITY_OFF offsetof(linux_fs_slice_hdr_t, fd_capacity)

DEFINE_PER_CPU(linux_fs_slice_hdr_t, linux_fs_hdr_scratch);
DEFINE_PER_CPU(linux_fs_slice_hdr_t, linux_fs_hdr_lookup_scratch);
DEFINE_PER_CPU(linux_fd_entry_t, linux_fd_entry_scratch);

static linux_fs_slice_hdr_t *linux_fs_hdr_buf(void)
{
        return &percpu(linux_fs_hdr_scratch);
}

static linux_fs_slice_hdr_t *linux_fs_hdr_lookup_buf(void)
{
        return &percpu(linux_fs_hdr_lookup_scratch);
}

static u64 linux_fs_fd_byte_off(i32 fd)
{
        return LINUX_FS_FD_REGION_BASE
               + (u64)fd * (u64)sizeof(linux_fd_entry_t);
}

static u64 linux_fs_table_byte_size(u32 fd_capacity)
{
        return linux_fs_fd_byte_off((i32)fd_capacity);
}

static error_t linux_fs_ensure_pgoff(struct page_slice *table, u64 pgoff)
{
        struct allocator *alloc = percpu(kallocator);
        struct page_slice_entry *entry;
        vaddr page;

        if (!table || !alloc) {
                return -E_IN_PARAM;
        }

        entry = page_slice_lookup(table, pgoff);
        if (entry) {
                return REND_SUCCESS;
        }

        page = (vaddr)alloc->m_alloc(alloc, PAGE_SIZE);
        if (!page) {
                return -E_REND_NO_MEM;
        }

        memset((void *)page, 0, PAGE_SIZE);
        return page_slice_insert_page(table, pgoff, page, 0);
}

static error_t linux_fs_ensure_bytes(struct page_slice *table, u64 byte_off,
                                       size_t len)
{
        u64 end;
        u64 pgoff;

        if (!table || len == 0) {
                return -E_IN_PARAM;
        }

        end = byte_off + (u64)len - 1ULL;
        for (pgoff = PAGE_SLICE_BYTE_TO_PGOFF(byte_off);
             pgoff <= PAGE_SLICE_BYTE_TO_PGOFF(end);
             pgoff++) {
                error_t err = linux_fs_ensure_pgoff(table, pgoff);

                if (err != REND_SUCCESS) {
                        return err;
                }
        }
        return REND_SUCCESS;
}

static error_t linux_fs_slice_load(struct page_slice *table, u64 byte_off,
                                   void *dst, size_t len)
{
        if (!table || !dst) {
                return -E_IN_PARAM;
        }
        return page_slice_copy_to_buffer(table, byte_off, dst, len);
}

static error_t linux_fs_slice_store(struct page_slice *table, u64 byte_off,
                                    const void *src, size_t len)
{
        size_t done = 0;
        error_t err;

        if (!table || !src) {
                return -E_IN_PARAM;
        }

        err = linux_fs_ensure_bytes(table, byte_off, len);
        if (err != REND_SUCCESS) {
                return err;
        }

        while (done < len) {
                u64 off = byte_off + (u64)done;
                u64 pgoff = PAGE_SLICE_BYTE_TO_PGOFF(off);
                u64 in_page = PAGE_SLICE_IN_PAGE_OFF(off);
                struct page_slice_entry *entry;
                size_t chunk;

                entry = page_slice_lookup(table, pgoff);
                if (!entry) {
                        return -E_RENDEZVOS;
                }

                chunk = PAGE_SIZE - (size_t)in_page;
                if (chunk > len - done) {
                        chunk = len - done;
                }

                memcpy((void *)(entry->kernel_virtual_address + in_page),
                       (const u8 *)src + done,
                       chunk);
                done += chunk;
        }
        return REND_SUCCESS;
}

static u32 linux_fs_table_fd_capacity(const struct page_slice *table)
{
        u32 cap = 0;

        if (!table) {
                return 0;
        }
        if (linux_fs_slice_load((struct page_slice *)table,
                                LINUX_FS_HDR_FD_CAPACITY_OFF,
                                &cap,
                                sizeof(cap))
            != REND_SUCCESS) {
                return 0;
        }
        return cap;
}

static error_t linux_fs_hdr_load(const linux_fs_state_t *fs,
                                 linux_fs_slice_hdr_t *hdr)
{
        if (!fs || !fs->table || !hdr) {
                return -E_IN_PARAM;
        }
        return linux_fs_slice_load(fs->table, 0, hdr, sizeof(*hdr));
}

static error_t linux_fs_hdr_store(linux_fs_state_t *fs,
                                  const linux_fs_slice_hdr_t *hdr)
{
        if (!fs || !fs->table || !hdr) {
                return -E_IN_PARAM;
        }
        return linux_fs_slice_store(fs->table, 0, hdr, sizeof(*hdr));
}

static error_t linux_fs_entry_load(const linux_fs_state_t *fs, i32 fd,
                                   linux_fd_entry_t *ent)
{
        if (!fs || !fs->table || !ent || fd < 0) {
                return -E_IN_PARAM;
        }
        return linux_fs_slice_load(fs->table,
                                   linux_fs_fd_byte_off(fd),
                                   ent,
                                   sizeof(*ent));
}

static error_t linux_fs_entry_store(linux_fs_state_t *fs, i32 fd,
                                    const linux_fd_entry_t *ent)
{
        if (!fs || !fs->table || !ent || fd < 0) {
                return -E_IN_PARAM;
        }
        return linux_fs_slice_store(fs->table,
                                    linux_fs_fd_byte_off(fd),
                                    ent,
                                    sizeof(*ent));
}

static error_t linux_fs_table_create(struct page_slice **table_out,
                                       u32 fd_capacity)
{
        struct page_slice *table;
        linux_fs_slice_hdr_t *hdr;
        u64 pgoff;
        u64 pg_count;
        error_t err;

        if (!table_out || fd_capacity == 0) {
                return -E_IN_PARAM;
        }

        *table_out = NULL;
        table = page_slice_create(0, linux_fs_table_byte_size(fd_capacity));
        if (!table) {
                return -E_REND_NO_MEM;
        }

        pg_count = PAGE_SLICE_SIZE_TO_PAGE_COUNT(
                page_slice_get_size(table));
        for (pgoff = 0; pgoff < pg_count; pgoff++) {
                err = linux_fs_ensure_pgoff(table, pgoff);
                if (err != REND_SUCCESS) {
                        page_slice_destroy(&table);
                        return err;
                }
        }

        hdr = linux_fs_hdr_buf();
        memset(hdr, 0, sizeof(*hdr));
        hdr->cwd[0] = '/';
        hdr->cwd[1] = '\0';
        hdr->fd_capacity = fd_capacity;

        err = linux_fs_slice_store(table, 0, hdr, sizeof(*hdr));
        if (err != REND_SUCCESS) {
                page_slice_destroy(&table);
                return err;
        }

        *table_out = table;
        return REND_SUCCESS;
}

static error_t linux_fs_grow_fd_cap(linux_fs_state_t *fs, u32 new_cap)
{
        linux_fs_slice_hdr_t *hdr;
        u64 old_size;
        u64 new_size;
        u64 pgoff;
        u64 old_pages;
        u64 new_pages;
        error_t err;

        if (!fs || !fs->table || new_cap == 0) {
                return -E_IN_PARAM;
        }

        hdr = linux_fs_hdr_buf();
        err = linux_fs_hdr_load(fs, hdr);
        if (err != REND_SUCCESS) {
                return err;
        }
        if (new_cap <= hdr->fd_capacity) {
                return REND_SUCCESS;
        }

        old_size = page_slice_get_size(fs->table);
        new_size = linux_fs_table_byte_size(new_cap);
        err = page_slice_set_size(&fs->table, new_size);
        if (err != REND_SUCCESS) {
                return err;
        }

        old_pages = PAGE_SLICE_SIZE_TO_PAGE_COUNT(old_size);
        new_pages = PAGE_SLICE_SIZE_TO_PAGE_COUNT(new_size);
        for (pgoff = old_pages; pgoff < new_pages; pgoff++) {
                err = linux_fs_ensure_pgoff(fs->table, pgoff);
                if (err != REND_SUCCESS) {
                        return err;
                }
        }

        hdr->fd_capacity = new_cap;
        return linux_fs_hdr_store(fs, hdr);
}

u32 linux_fs_fd_capacity(const linux_fs_state_t *fs)
{
        if (!fs || !fs->table) {
                return 0;
        }
        return linux_fs_table_fd_capacity(fs->table);
}

const char *linux_fs_cwd(const linux_fs_state_t *fs)
{
        linux_fs_slice_hdr_t *hdr;

        if (!fs || !fs->table) {
                return "/";
        }
        hdr = linux_fs_hdr_lookup_buf();
        if (linux_fs_hdr_load(fs, hdr) != REND_SUCCESS) {
                return "/";
        }
        return hdr->cwd;
}

void linux_fs_set_cwd(linux_fs_state_t *fs, const char *cwd)
{
        linux_fs_slice_hdr_t *hdr;

        if (!fs || !fs->table || !cwd) {
                return;
        }
        hdr = linux_fs_hdr_buf();
        if (linux_fs_hdr_load(fs, hdr) != REND_SUCCESS) {
                return;
        }

        strncpy(hdr->cwd, cwd, sizeof(hdr->cwd) - 1);
        hdr->cwd[sizeof(hdr->cwd) - 1] = '\0';
        (void)linux_fs_hdr_store(fs, hdr);
}

static void linux_fs_dir_paths_clear_all(linux_fs_state_t *fs)
{
        (void)fs;
}

static i64 linux_fs_dir_path_store(linux_fs_state_t *fs, i32 fd,
                                   const char *path)
{
        if (!fs || !fs->table || fd < 0 || !path) {
                return -LINUX_EINVAL;
        }
        if ((u32)fd >= linux_fs_fd_capacity(fs)) {
                return -LINUX_EINVAL;
        }

        linux_fd_set_vfs_abs_path(fs, fd, path);
        linux_fd_set_is_dir(fs, fd, true);
        return 0;
}

static void linux_fs_dir_path_release(linux_fs_state_t *fs, i32 fd)
{
        linux_fd_entry_t ent;

        if (!fs || !fs->table || fd < 0) {
                return;
        }
        if (linux_fs_entry_load(fs, fd, &ent) != REND_SUCCESS) {
                return;
        }
        if (!ent.is_dir) {
                return;
        }

        ent.vfs_abs_path[0] = '\0';
        (void)linux_fs_entry_store(fs, fd, &ent);
}

const char *linux_fs_dir_path_lookup(linux_fs_state_t *fs, i32 fd)
{
        linux_fd_entry_t ent;

        if (!fs || !fs->table || fd < 0) {
                return NULL;
        }

        if (linux_fs_entry_load(fs, fd, &ent) != REND_SUCCESS) {
                return NULL;
        }
        if (!ent.is_dir || ent.vfs_abs_path[0] == '\0') {
                return NULL;
        }

        /*
         * Return path in per-CPU lookup hdr scratch (same contract as cwd).
         * Caller must copy if the pointer must survive another fs helper call.
         */
        {
                linux_fs_slice_hdr_t *scratch = linux_fs_hdr_lookup_buf();

                strncpy(scratch->cwd, ent.vfs_abs_path, sizeof(scratch->cwd) - 1);
                scratch->cwd[sizeof(scratch->cwd) - 1] = '\0';
                return scratch->cwd;
        }
}

i64 linux_fs_dir_path_assign(linux_fs_state_t *fs, i32 fd, const char *path)
{
        return linux_fs_dir_path_store(fs, fd, path);
}

void linux_fs_dir_path_dup(linux_fs_state_t *fs, i32 oldfd, i32 newfd)
{
        linux_fd_entry_t oldent;

        if (!fs || oldfd == newfd) {
                return;
        }

        if (linux_fs_entry_load(fs, oldfd, &oldent) != REND_SUCCESS
            || !oldent.is_dir || oldent.vfs_abs_path[0] == '\0') {
                linux_fs_dir_path_release(fs, newfd);
                return;
        }

        (void)linux_fs_entry_store(fs, newfd, &oldent);
}

void linux_fd_set_vfs_abs_path(linux_fs_state_t *fs, i32 fd, const char *path)
{
        linux_fd_entry_t ent;

        if (!fs || fd < 0 || !path || (u32)fd >= linux_fs_fd_capacity(fs)) {
                return;
        }
        if (linux_fs_entry_load(fs, fd, &ent) != REND_SUCCESS) {
                return;
        }

        strncpy(ent.vfs_abs_path, path, sizeof(ent.vfs_abs_path) - 1);
        ent.vfs_abs_path[sizeof(ent.vfs_abs_path) - 1] = '\0';
        (void)linux_fs_entry_store(fs, fd, &ent);
}

void linux_fd_set_is_dir(linux_fs_state_t *fs, i32 fd, bool is_dir)
{
        linux_fd_entry_t ent;

        if (!fs || fd < 0 || (u32)fd >= linux_fs_fd_capacity(fs)) {
                return;
        }
        if (linux_fs_entry_load(fs, fd, &ent) != REND_SUCCESS) {
                return;
        }

        ent.is_dir = is_dir;
        (void)linux_fs_entry_store(fs, fd, &ent);
}

linux_fs_state_t *linux_fs_state(Tcb_Base *task)
{
        linux_proc_append_t *pa;

        if (!task) {
                return NULL;
        }

        pa = linux_proc_append(task);
        if (!pa) {
                return NULL;
        }

        return pa->fs;
}

static linux_fs_state_t *linux_fs_alloc_state(void)
{
        struct allocator *alloc = percpu(kallocator);
        linux_fs_state_t *fs;

        if (!alloc) {
                return NULL;
        }

        fs = (linux_fs_state_t *)alloc->m_alloc(alloc, sizeof(*fs));
        if (!fs) {
                return NULL;
        }

        fs->table = NULL;
        return fs;
}

static void linux_fs_free_state(linux_fs_state_t *fs)
{
        struct allocator *alloc = percpu(kallocator);

        if (!fs || !alloc) {
                return;
        }

        if (fs->table) {
                page_slice_destroy(&fs->table);
        }
        alloc->m_free(alloc, fs);
}

static error_t linux_fs_init_state(linux_fs_state_t *fs)
{
        linux_fd_entry_t ent;
        error_t err;

        if (!fs) {
                return -E_IN_PARAM;
        }

        if (fs->table) {
                page_slice_destroy(&fs->table);
                fs->table = NULL;
        }

        err = linux_fs_table_create(&fs->table, LINUX_FS_FD_INIT_CAP);
        if (err != REND_SUCCESS) {
                return err;
        }

        linux_fs_dir_paths_clear_all(fs);

        memset(&ent, 0, sizeof(ent));
        ent.kind = LINUX_FD_CONSOLE_IN;
        err = linux_fs_entry_store(fs, 0, &ent);
        if (err != REND_SUCCESS) {
                return err;
        }

        ent.kind = LINUX_FD_CONSOLE_OUT;
        err = linux_fs_entry_store(fs, 1, &ent);
        if (err != REND_SUCCESS) {
                return err;
        }

        ent.kind = LINUX_FD_CONSOLE_ERR;
        return linux_fs_entry_store(fs, 2, &ent);
}

error_t linux_fs_proc_attach(Tcb_Base *task)
{
        linux_proc_append_t *pa;
        linux_fs_state_t *fs;
        error_t err;

        if (!task) {
                return -E_IN_PARAM;
        }

        pa = linux_proc_append(task);
        if (!pa) {
                return -E_IN_PARAM;
        }

        if (pa->fs) {
                return linux_fs_init_state(pa->fs);
        }

        fs = linux_fs_alloc_state();
        if (!fs) {
                return -E_RENDEZVOS;
        }

        err = linux_fs_init_state(fs);
        if (err != REND_SUCCESS) {
                linux_fs_free_state(fs);
                return err;
        }

        pa->fs = fs;
        return REND_SUCCESS;
}

static void linux_fs_release_vfs_handle(u32 handle)
{
        if (handle != 0) {
                (void)vfs_ipc_request_response(
                        KMSG_OP_VFS_CLOSE, VFS_KMSG_FMT_CLOSE, handle);
        }
}

static void linux_fs_retain_vfs_handle(u32 handle)
{
        if (handle != 0) {
                (void)vfs_ipc_request_response(KMSG_OP_VFS_HANDLE_RETAIN,
                                               VFS_KMSG_FMT_HANDLE_RETAIN,
                                               handle);
        }
}

static void linux_fs_release_open_resources(linux_fs_state_t *fs)
{
        u32 seen[VFS_HANDLE_MAX];
        u32 cap;
        u32 i;
        linux_fd_entry_t ent;

        if (!fs || !fs->table) {
                return;
        }

        memset(seen, 0, sizeof(seen));
        cap = linux_fs_fd_capacity(fs);

        for (i = 0; i < cap; i++) {
                if (linux_fs_entry_load(fs, (i32)i, &ent) != REND_SUCCESS) {
                        continue;
                }
                if (ent.kind == LINUX_FD_PIPE) {
                        linux_pipe_fd_closed(ent.vfs_handle, ent.pipe_read);
                        continue;
                }
                if (ent.kind != LINUX_FD_VFS || ent.vfs_handle == 0) {
                        continue;
                }
                if (ent.is_dir) {
                        linux_fs_dir_path_release(fs, (i32)i);
                }
                if (ent.vfs_handle < VFS_HANDLE_MAX
                    && !seen[ent.vfs_handle]) {
                        seen[ent.vfs_handle] = 1;
                        linux_fs_release_vfs_handle(ent.vfs_handle);
                }
        }
}

static void linux_fs_reset_state(linux_fs_state_t *fs)
{
        if (!fs || !fs->table) {
                return;
        }

        linux_fs_release_open_resources(fs);
        (void)linux_fs_init_state(fs);
}

void linux_fs_proc_release_for_exit(Tcb_Base *task)
{
        linux_fs_state_t *fs = linux_fs_state(task);

        if (!fs) {
                return;
        }

        /*
         * sys_exit: drop VFS/pipe references only. Do not rebuild page_slice
         * here — delete_task append fini will destroy fs after vspace teardown.
         */
        linux_fs_release_open_resources(fs);
}

void linux_fs_proc_reset(Tcb_Base *task)
{
        linux_fs_state_t *fs = linux_fs_state(task);

        if (!fs) {
                (void)linux_fs_proc_attach(task);
                return;
        }

        linux_fs_reset_state(fs);
}

void linux_fs_proc_destroy(Tcb_Base *task)
{
        linux_proc_append_t *pa;
        linux_fs_state_t *fs;

        if (!task) {
                return;
        }

        pa = linux_proc_append(task);
        if (!pa || !pa->fs) {
                return;
        }

        fs = pa->fs;
        linux_fs_release_open_resources(fs);
        linux_fs_free_state(fs);
        pa->fs = NULL;
}

static void linux_fs_fork_retain_resources(linux_fs_state_t *fs)
{
        u32 seen[VFS_HANDLE_MAX];
        u32 cap;
        u32 i;
        linux_fd_entry_t ent;

        if (!fs || !fs->table) {
                return;
        }

        memset(seen, 0, sizeof(seen));
        cap = linux_fs_fd_capacity(fs);

        for (i = 0; i < cap; i++) {
                if (linux_fs_entry_load(fs, (i32)i, &ent) != REND_SUCCESS) {
                        continue;
                }
                if (ent.kind == LINUX_FD_PIPE) {
                        linux_pipe_fork_retain(ent.vfs_handle);
                        continue;
                }
                if (ent.kind != LINUX_FD_VFS || ent.vfs_handle == 0) {
                        continue;
                }
                if (ent.vfs_handle < VFS_HANDLE_MAX
                    && !seen[ent.vfs_handle]) {
                        seen[ent.vfs_handle] = 1;
                        linux_fs_retain_vfs_handle(ent.vfs_handle);
                }
        }
}

static error_t linux_fs_fork_copy_state(linux_fs_state_t *child,
                                        const linux_fs_state_t *parent)
{
        error_t err;

        if (!child || !parent || !parent->table) {
                return -E_IN_PARAM;
        }

        if (child->table) {
                page_slice_destroy(&child->table);
                child->table = NULL;
        }

        err = page_slice_clone(&child->table, parent->table);
        if (err != REND_SUCCESS) {
                return err;
        }

        linux_fs_fork_retain_resources(child);
        return REND_SUCCESS;
}

error_t linux_fs_proc_fork(Tcb_Base *child, Tcb_Base *parent)
{
        linux_proc_append_t *pa;
        linux_fs_state_t *parent_fs;
        linux_fs_state_t *child_fs;
        error_t e;

        if (!child || !parent) {
                return -E_IN_PARAM;
        }

        pa = linux_proc_append(child);
        if (!pa) {
                return -E_IN_PARAM;
        }

        parent_fs = linux_fs_state(parent);

        if (pa->fs) {
                linux_fs_free_state(pa->fs);
                pa->fs = NULL;
        }

        child_fs = linux_fs_alloc_state();
        if (!child_fs) {
                return -E_RENDEZVOS;
        }

        if (parent_fs && parent_fs->table) {
                e = linux_fs_fork_copy_state(child_fs, parent_fs);
        } else {
                e = linux_fs_init_state(child_fs);
        }
        if (e != REND_SUCCESS) {
                linux_fs_free_state(child_fs);
                return e;
        }

        pa->fs = child_fs;
        return REND_SUCCESS;
}

bool linux_fs_handle_in_use(const linux_fs_state_t *fs, u32 handle)
{
        u32 cap;
        u32 i;
        linux_fd_entry_t ent;

        if (!fs || !fs->table || handle == 0) {
                return false;
        }

        cap = linux_fs_fd_capacity(fs);
        for (i = 0; i < cap; i++) {
                if (linux_fs_entry_load(fs, (i32)i, &ent) != REND_SUCCESS) {
                        continue;
                }
                if (ent.kind == LINUX_FD_VFS && ent.vfs_handle == handle) {
                        return true;
                }
        }

        return false;
}

linux_fd_entry_t *linux_fd_get(Tcb_Base *task, i32 fd)
{
        linux_fd_entry_t *ent;
        linux_fs_state_t *fs;

        if (fd < 0) {
                return NULL;
        }

        fs = linux_fs_state(task);
        if (!fs || !fs->table || (u32)fd >= linux_fs_fd_capacity(fs)) {
                return NULL;
        }
        ent = &percpu(linux_fd_entry_scratch);
        if (linux_fs_entry_load(fs, fd, ent) != REND_SUCCESS) {
                return NULL;
        }
        if (ent->kind == LINUX_FD_NONE) {
                return NULL;
        }

        return ent;
}

static i32 linux_fd_find_free(linux_fs_state_t *fs)
{
        u32 cap;
        u32 fd;
        linux_fd_entry_t ent;

        if (!fs || !fs->table) {
                return -1;
        }

        cap = linux_fs_fd_capacity(fs);
        for (fd = 0; fd < cap; fd++) {
                if (linux_fs_entry_load(fs, (i32)fd, &ent) != REND_SUCCESS) {
                        continue;
                }
                if (ent.kind == LINUX_FD_NONE) {
                        return (i32)fd;
                }
        }
        return -1;
}

i32 linux_fd_alloc(Tcb_Base *task, const linux_fd_entry_t *ent_in)
{
        linux_fs_state_t *fs;
        i32 fd;
        error_t err;

        if (!task || !ent_in) {
                return -1;
        }

        fs = linux_fs_state(task);
        if (!fs || !fs->table) {
                return -1;
        }

        fd = linux_fd_find_free(fs);
        if (fd < 0) {
                u32 new_cap = linux_fs_fd_capacity(fs) + LINUX_FS_SLICE_FD_GROW;

                err = linux_fs_grow_fd_cap(fs, new_cap);
                if (err != REND_SUCCESS) {
                        return -1;
                }
                fd = linux_fd_find_free(fs);
                if (fd < 0) {
                        return -1;
                }
        }

        if (linux_fs_entry_store(fs, fd, ent_in) != REND_SUCCESS) {
                return -1;
        }

        return fd;
}

i32 linux_fd_lowest_free(Tcb_Base *task)
{
        linux_fs_state_t *fs = linux_fs_state(task);
        i32 fd;
        error_t err;

        if (!fs) {
                return -1;
        }

        fd = linux_fd_find_free(fs);
        if (fd >= 0) {
                return fd;
        }

        err = linux_fs_grow_fd_cap(fs,
                                   linux_fs_fd_capacity(fs)
                                           + LINUX_FS_SLICE_FD_GROW);
        if (err != REND_SUCCESS) {
                return -1;
        }

        return linux_fd_find_free(fs);
}

i64 linux_fd_close(Tcb_Base *task, i32 fd)
{
        linux_fs_state_t *fs;
        linux_fd_entry_t ent;
        u32 handle;

        if (fd < 0) {
                return -LINUX_EBADF;
        }

        fs = linux_fs_state(task);
        if (!fs || !fs->table) {
                return -LINUX_ESRCH;
        }
        if ((u32)fd >= linux_fs_fd_capacity(fs)) {
                return -LINUX_EBADF;
        }
        if (linux_fs_entry_load(fs, fd, &ent) != REND_SUCCESS) {
                return -LINUX_EBADF;
        }
        if (ent.kind == LINUX_FD_NONE) {
                return -LINUX_EBADF;
        }

        handle = ent.vfs_handle;
        if (ent.kind == LINUX_FD_PIPE) {
                linux_pipe_fd_closed(handle, ent.pipe_read);
        } else if (ent.kind == LINUX_FD_VFS) {
                if (ent.is_dir) {
                        linux_fs_dir_path_release(fs, fd);
                }
                ent.kind = LINUX_FD_NONE;
                ent.vfs_handle = 0;
                ent.is_dir = false;
                ent.pipe_read = false;
                ent.vfs_abs_path[0] = '\0';
                (void)linux_fs_entry_store(fs, fd, &ent);
                if (handle != 0 && !linux_fs_handle_in_use(fs, handle)) {
                        linux_fs_release_vfs_handle(handle);
                }
                return 0;
        }

        ent.kind = LINUX_FD_NONE;
        ent.vfs_handle = 0;
        ent.is_dir = false;
        ent.pipe_read = false;
        ent.vfs_abs_path[0] = '\0';
        (void)linux_fs_entry_store(fs, fd, &ent);

        return 0;
}

i64 linux_fd_dup2(Tcb_Base *task, i32 oldfd, i32 newfd)
{
        linux_fs_state_t *fs;
        linux_fd_entry_t oldent;
        linux_fd_entry_t newent;
        u32 replaced_handle;

        if (oldfd < 0 || newfd < 0) {
                return -LINUX_EBADF;
        }

        if (oldfd == newfd) {
                if (!linux_fd_get(task, oldfd)) {
                        return -LINUX_EBADF;
                }
                return (i64)newfd;
        }

        fs = linux_fs_state(task);
        if (!fs || !fs->table) {
                return -LINUX_ESRCH;
        }
        if ((u32)newfd >= linux_fs_fd_capacity(fs)) {
                error_t grow_err = linux_fs_grow_fd_cap(fs, (u32)newfd + 1u);

                if (grow_err != REND_SUCCESS) {
                        return -LINUX_EBADF;
                }
        }
        if ((u32)oldfd >= linux_fs_fd_capacity(fs)) {
                return -LINUX_EBADF;
        }
        if (linux_fs_entry_load(fs, oldfd, &oldent) != REND_SUCCESS
            || oldent.kind == LINUX_FD_NONE) {
                return -LINUX_EBADF;
        }
        if (linux_fs_entry_load(fs, newfd, &newent) != REND_SUCCESS) {
                return -LINUX_EBADF;
        }

        if (newent.kind == LINUX_FD_PIPE) {
                linux_pipe_fd_closed(newent.vfs_handle, newent.pipe_read);
        }

        replaced_handle = 0;
        if (newent.kind == LINUX_FD_VFS) {
                replaced_handle = newent.vfs_handle;
        }

        if (linux_fs_entry_store(fs, newfd, &oldent) != REND_SUCCESS) {
                return -LINUX_EBADF;
        }
        newent = oldent;

        if (newent.is_dir) {
                linux_fs_dir_path_dup(fs, oldfd, newfd);
        } else {
                linux_fs_dir_path_release(fs, newfd);
        }

        if (newent.kind == LINUX_FD_VFS && newent.vfs_handle != 0) {
                linux_fs_retain_vfs_handle(newent.vfs_handle);
        } else if (newent.kind == LINUX_FD_PIPE) {
                linux_pipe_fork_retain(newent.vfs_handle);
        }

        if (replaced_handle != 0
            && !linux_fs_handle_in_use(fs, replaced_handle)) {
                linux_fs_release_vfs_handle(replaced_handle);
        }

        return (i64)newfd;
}
