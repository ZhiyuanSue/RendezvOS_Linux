#ifndef _LINUX_COMPAT_TEST_MANIFEST_H_
#define _LINUX_COMPAT_TEST_MANIFEST_H_

#include <common/types.h>

#define LINUX_USER_TEST_MAX      128
#define LINUX_USER_TEST_PATH_MAX 128

#define LINUX_USER_TEST_MANIFEST_PATH "/tests/manifest"

/*
 * Load /tests/manifest from initramfs (cpio). Each non-empty line is a VFS
 * path to a static ELF64 test binary.
 *
 * Returns 0 on success, negative Linux errno on failure.
 */
i64 linux_user_test_load_manifest(void);

u32 linux_user_test_count(void);

const char *linux_user_test_path(u32 index);

#endif /* _LINUX_COMPAT_TEST_MANIFEST_H_ */
