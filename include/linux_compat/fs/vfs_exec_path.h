#ifndef _LINUX_COMPAT_FS_VFS_EXEC_PATH_H_
#define _LINUX_COMPAT_FS_VFS_EXEC_PATH_H_

#include <common/stdbool.h>
#include <common/types.h>

#define LINUX_USER_TEST_VFS_PREFIX "/tests/"

/*
 * Map a user execve path to initramfs layout.
 * e.g. "test_echo" or "/test_echo" -> "/tests/test_echo"
 * Returns false if @p out is too small or @p path is invalid.
 */
/*
 * Map a user execve path to initramfs layout.
 * e.g. "test_echo" or "/test_echo" -> "/tests/test_echo"
 * Returns false if @p out is too small or @p path is invalid.
 */
bool linux_vfs_exec_path_under_tests(const char *path, char *out, u64 out_cap);

const char *linux_vfs_exec_path_basename(const char *path);

#endif /* _LINUX_COMPAT_FS_VFS_EXEC_PATH_H_ */
