/*
 * In-process pipe buffers for pipe2 (compat layer, scheme B).
 *
 * Open ends are counted (not bools): after fork both parent and child hold
 * read+write fds, so one close must not mark the whole pipe EOF/EPIPE.
 */

#include <linux_compat/errno.h>
#include <linux_compat/fs/linux_fcntl.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/linux_pipe.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>

#include <common/string.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>

#define LINUX_PIPE_BUF_SIZE 4096u
#define LINUX_PIPE_MAX      64u

typedef struct linux_pipe {
        u8 data[LINUX_PIPE_BUF_SIZE];
        u32 head;
        u32 len;
        u32 refcnt; /* open fds (read ends + write ends) */
        u32 readers; /* open read-end fds */
        u32 writers; /* open write-end fds */
} linux_pipe_t;

static linux_pipe_t *linux_pipes[LINUX_PIPE_MAX];

static struct allocator *linux_pipe_alloc(void)
{
        return percpu(kallocator);
}

static u32 linux_pipe_alloc_slot(void)
{
        struct allocator *alloc = linux_pipe_alloc();
        linux_pipe_t *pipe;
        u32 i;

        if (!alloc) {
                return LINUX_PIPE_MAX;
        }

        for (i = 0; i < LINUX_PIPE_MAX; i++) {
                if (linux_pipes[i] == NULL) {
                        pipe = (linux_pipe_t *)alloc->m_alloc(alloc,
                                                              sizeof(*pipe));
                        if (!pipe) {
                                return LINUX_PIPE_MAX;
                        }
                        memset(pipe, 0, sizeof(*pipe));
                        pipe->refcnt = 2;
                        pipe->readers = 1;
                        pipe->writers = 1;
                        linux_pipes[i] = pipe;
                        return i;
                }
        }

        return LINUX_PIPE_MAX;
}

static linux_pipe_t *linux_pipe_from_id(u32 id)
{
        if (id >= LINUX_PIPE_MAX) {
                return NULL;
        }

        return linux_pipes[id];
}

static void linux_pipe_free_slot(u32 id)
{
        struct allocator *alloc = linux_pipe_alloc();
        linux_pipe_t *pipe = linux_pipe_from_id(id);

        if (!pipe) {
                return;
        }

        linux_pipes[id] = NULL;
        if (alloc) {
                alloc->m_free(alloc, pipe);
        }
}

static void linux_pipe_release(u32 id)
{
        linux_pipe_t *pipe = linux_pipe_from_id(id);

        if (!pipe) {
                return;
        }

        if (pipe->refcnt == 0 || --pipe->refcnt > 0) {
                return;
        }

        linux_pipe_free_slot(id);
}

void linux_pipe_fork_retain(u32 pipe_id, bool read_end)
{
        linux_pipe_t *pipe = linux_pipe_from_id(pipe_id);

        if (!pipe) {
                return;
        }

        pipe->refcnt++;
        if (read_end) {
                pipe->readers++;
        } else {
                pipe->writers++;
        }
}

void linux_pipe_fd_closed(u32 pipe_id, bool read_end)
{
        linux_pipe_t *pipe = linux_pipe_from_id(pipe_id);

        if (!pipe) {
                return;
        }

        if (read_end) {
                if (pipe->readers > 0) {
                        pipe->readers--;
                }
        } else {
                if (pipe->writers > 0) {
                        pipe->writers--;
                }
        }

        linux_pipe_release(pipe_id);
}

i64 linux_pipe_create2(linux_proc_resource_t *task, u64 user_pipefd, i32 flags)
{
        linux_fd_entry_t read_ent;
        linux_fd_entry_t write_ent;
        i32 read_fd;
        i32 write_fd;
        u32 pipe_id;
        i32 fds[2];
        error_t e;
        u32 fd_flags = 0;
        u32 open_flags = 0;

        if ((flags & ~(LINUX_O_CLOEXEC | LINUX_O_NONBLOCK)) != 0) {
                return -LINUX_EINVAL;
        }
        if ((flags & LINUX_O_CLOEXEC) != 0) {
                fd_flags = LINUX_FD_CLOEXEC;
        }
        if ((flags & LINUX_O_NONBLOCK) != 0) {
                open_flags |= LINUX_O_NONBLOCK;
        }

        if (!task || !linux_current_vs()) {
                return -LINUX_EFAULT;
        }

        pipe_id = linux_pipe_alloc_slot();
        if (pipe_id >= LINUX_PIPE_MAX) {
                return -LINUX_EMFILE;
        }

        memset(&read_ent, 0, sizeof(read_ent));
        read_ent.kind = LINUX_FD_PIPE;
        read_ent.vfs_handle = pipe_id;
        read_ent.pipe_read = true;
        read_ent.open_flags = open_flags;
        read_ent.fd_flags = fd_flags;

        memset(&write_ent, 0, sizeof(write_ent));
        write_ent.kind = LINUX_FD_PIPE;
        write_ent.vfs_handle = pipe_id;
        write_ent.pipe_read = false;
        write_ent.open_flags = open_flags;
        write_ent.fd_flags = fd_flags;

        read_fd = linux_fd_alloc(task, &read_ent);
        if (read_fd < 0) {
                linux_pipe_free_slot(pipe_id);
                return -LINUX_EMFILE;
        }

        write_fd = linux_fd_alloc(task, &write_ent);
        if (write_fd < 0) {
                (void)linux_fd_close(task, read_fd);
                linux_pipe_free_slot(pipe_id);
                return -LINUX_EMFILE;
        }

        fds[0] = read_fd;
        fds[1] = write_fd;

        e = linux_mm_store_to_user(linux_current_vs(), user_pipefd, fds, sizeof(fds));
        if (e != REND_SUCCESS) {
                (void)linux_fd_close(task, write_fd);
                (void)linux_fd_close(task, read_fd);
                return -LINUX_EFAULT;
        }

        return 0;
}

i64 linux_pipe_read(linux_proc_resource_t *task, u32 pipe_id, u64 user_buf, u64 count)
{
        linux_pipe_t *pipe = linux_pipe_from_id(pipe_id);
        u8 chunk[256];
        u64 total = 0;

        if (!task || !linux_current_vs() || !pipe || pipe->readers == 0) {
                return -LINUX_EBADF;
        }

        if (count == 0) {
                return 0;
        }

        while (total < count) {
                if (pipe->len == 0) {
                        if (pipe->writers == 0) {
                                break;
                        }
                        schedule(percpu(core_tm));
                        continue;
                }

                u64 chunk_len = count - total;
                u32 avail = pipe->len;
                u32 off = pipe->head;
                u32 n;

                if (chunk_len > sizeof(chunk)) {
                        chunk_len = sizeof(chunk);
                }
                if (chunk_len > avail) {
                        chunk_len = avail;
                }

                n = (u32)chunk_len;
                memcpy(chunk, pipe->data + off, n);
                if (linux_mm_store_to_user(linux_current_vs(), user_buf + total, chunk, n)
                    != REND_SUCCESS) {
                        return total > 0 ? (i64)total : -LINUX_EFAULT;
                }

                pipe->head = (pipe->head + n) % LINUX_PIPE_BUF_SIZE;
                pipe->len -= n;
                total += n;
        }

        if (total == 0 && pipe->writers == 0) {
                return 0;
        }

        return (i64)total;
}

i64 linux_pipe_write(linux_proc_resource_t *task, u32 pipe_id, u64 user_buf, u64 count)
{
        linux_pipe_t *pipe = linux_pipe_from_id(pipe_id);
        u8 chunk[256];
        u64 total = 0;

        if (!task || !linux_current_vs() || !pipe || pipe->writers == 0) {
                return -LINUX_EBADF;
        }

        if (count == 0) {
                return 0;
        }

        while (total < count) {
                u64 chunk_len = count - total;
                u32 space;
                u32 tail;
                u32 n;

                if (pipe->len >= LINUX_PIPE_BUF_SIZE) {
                        if (pipe->readers == 0) {
                                break;
                        }
                        schedule(percpu(core_tm));
                        continue;
                }

                space = LINUX_PIPE_BUF_SIZE - pipe->len;
                if (chunk_len > sizeof(chunk)) {
                        chunk_len = sizeof(chunk);
                }
                if (chunk_len > space) {
                        chunk_len = space;
                }

                n = (u32)chunk_len;
                if (linux_mm_load_from_user(linux_current_vs(), user_buf + total, chunk, n)
                    != REND_SUCCESS) {
                        return total > 0 ? (i64)total : -LINUX_EFAULT;
                }

                tail = (pipe->head + pipe->len) % LINUX_PIPE_BUF_SIZE;
                if (tail + n <= LINUX_PIPE_BUF_SIZE) {
                        memcpy(pipe->data + tail, chunk, n);
                } else {
                        u32 first = LINUX_PIPE_BUF_SIZE - tail;

                        memcpy(pipe->data + tail, chunk, first);
                        memcpy(pipe->data, chunk + first, n - first);
                }

                pipe->len += n;
                total += n;
        }

        if (total == 0 && pipe->readers == 0) {
                return -LINUX_EPIPE;
        }

        return (i64)total;
}
