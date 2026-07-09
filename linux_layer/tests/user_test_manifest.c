/*
 * Parse /tests/manifest from initramfs for the user test harness.
 */

#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_kern_load.h>
#include <linux_compat/test_manifest.h>

#include <common/mm.h>
#include <common/stdbool.h>
#include <common/string.h>
#include <common/types.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/page_slice_copy.h>
#include <rendezvos/smp/percpu.h>

static char linux_user_test_paths[LINUX_USER_TEST_MAX][LINUX_USER_TEST_PATH_MAX];
static u32 linux_user_test_manifest_count;

static bool manifest_line_valid(const char *line, u64 len)
{
        u64 i;

        if (len == 0 || len >= LINUX_USER_TEST_PATH_MAX) {
                return false;
        }
        if (line[0] != '/') {
                return false;
        }
        for (i = 0; i < len; i++) {
                if (line[i] == '\0' || line[i] == '\r' || line[i] == '\n') {
                        break;
                }
        }
        return i == len;
}

static i64 manifest_slice_byte(struct page_slice *slice, u64 off, u8 *out)
{
        struct page_slice_entry *entry;
        u64 pgoff = PAGE_SLICE_BYTE_TO_PGOFF(off);
        u64 in_page = PAGE_SLICE_IN_PAGE_OFF(off);

        entry = page_slice_lookup(slice, pgoff);
        if (!entry || !out) {
                return -LINUX_EIO;
        }

        *out = ((const u8 *)entry->kernel_virtual_address)[in_page];
        return 0;
}

static i64 manifest_trim_line_end(struct page_slice *slice, u64 line_start,
                                  u64 line_len)
{
        u8 c;

        while (line_len > 0) {
                i64 ret = manifest_slice_byte(slice, line_start + line_len - 1,
                                              &c);
                if (ret != 0) {
                        return ret;
                }
                if (c != '\r' && c != ' ' && c != '\t') {
                        break;
                }
                line_len--;
        }

        return (i64)line_len;
}

i64 linux_user_test_load_manifest(void)
{
        struct allocator *alloc = percpu(kallocator);
        struct page_slice *slice = NULL;
        u64 size;
        u64 i = 0;
        i64 ret;

        linux_user_test_manifest_count = 0;
        if (!alloc) {
                return -LINUX_ENOMEM;
        }

        ret = vfs_kern_read_file_slice(LINUX_USER_TEST_MANIFEST_PATH, alloc,
                                       &slice);
        if (ret != 0) {
                return ret;
        }

        size = page_slice_get_size(slice);
        while (i < size && linux_user_test_manifest_count < LINUX_USER_TEST_MAX) {
                u64 line_start = i;
                u64 line_len = 0;
                char line_buf[LINUX_USER_TEST_PATH_MAX];
                u8 c;

                while (i < size) {
                        ret = manifest_slice_byte(slice, i, &c);
                        if (ret != 0) {
                                goto out_destroy;
                        }
                        i++;
                        if (c == '\n') {
                                break;
                        }
                        line_len++;
                }

                ret = manifest_trim_line_end(slice, line_start, line_len);
                if (ret < 0) {
                        goto out_destroy;
                }
                line_len = (u64)ret;

                if (line_len == 0) {
                        continue;
                }
                if (line_len >= LINUX_USER_TEST_PATH_MAX) {
                        ret = -LINUX_EINVAL;
                        goto out_destroy;
                }

                ret = manifest_slice_byte(slice, line_start, &c);
                if (ret != 0) {
                        goto out_destroy;
                }
                if (c == '#') {
                        continue;
                }

                if (page_slice_copy_to_buffer(slice, line_start, line_buf,
                                              (size_t)line_len)
                    != REND_SUCCESS) {
                        ret = -LINUX_EIO;
                        goto out_destroy;
                }
                line_buf[line_len] = '\0';
                if (!manifest_line_valid(line_buf, line_len)) {
                        ret = -LINUX_EINVAL;
                        goto out_destroy;
                }

                memcpy(linux_user_test_paths[linux_user_test_manifest_count],
                       line_buf,
                       (size_t)line_len + 1);
                linux_user_test_manifest_count++;
        }

        page_slice_destroy(&slice);

        if (linux_user_test_manifest_count == 0) {
                return -LINUX_EINVAL;
        }

        return 0;

out_destroy:
        page_slice_destroy(&slice);
        return ret;
}

u32 linux_user_test_count(void)
{
        return linux_user_test_manifest_count;
}

const char *linux_user_test_path(u32 index)
{
        if (index >= linux_user_test_manifest_count) {
                return NULL;
        }
        return linux_user_test_paths[index];
}
