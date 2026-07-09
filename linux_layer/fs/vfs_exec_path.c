#include <linux_compat/fs/vfs_exec_path.h>

#include <common/string.h>

static const char *exec_path_basename(const char *path)
{
        const char *base = path;

        if (!path) {
                return "";
        }

        while (*path) {
                if (*path == '/') {
                        base = path + 1;
                }
                path++;
        }

        return base;
}

const char *linux_vfs_exec_path_basename(const char *path)
{
        return exec_path_basename(path);
}

bool linux_vfs_exec_path_under_tests(const char *path, char *out, u64 out_cap)
{
        const char *base;
        u64 prefix_len;
        u64 base_len;
        u64 total;

        if (!path || !out || out_cap == 0) {
                return false;
        }

        base = exec_path_basename(path);
        if (base[0] == '\0') {
                return false;
        }

        prefix_len = strlen(LINUX_USER_TEST_VFS_PREFIX);
        base_len = strlen(base);
        total = prefix_len + base_len;

        if (total + 1 > out_cap) {
                return false;
        }

        memcpy(out, LINUX_USER_TEST_VFS_PREFIX, prefix_len);
        memcpy(out + prefix_len, base, base_len);
        out[total] = '\0';
        return true;
}
