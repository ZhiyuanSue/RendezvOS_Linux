#include "vfs_backend.h"

#include "vfs_backend_ipc.h"
#include "vfs_mount.h"
#include "vfs_slice_table.h"

#include <common/string.h>
#include <linux_compat/errno.h>

typedef struct vfs_backend_entry {
        const char *port_name;
        char fstype[VFS_BACKEND_FSTYPE_MAX];
        u32 caps;
        u32 reg_flags;
        bool active;
} vfs_backend_entry_t;

static vfs_slice_table_t vfs_backend_tab;
static const char *vfs_backend_root_port_ptr;
static const char *vfs_backend_overlay_port_ptr;
static u32 vfs_backend_online_flags;
static bool vfs_backend_tab_ready;

#define VFS_BACKEND_BOOT_IO_READY \
        (VFS_BACKEND_REG_ROOT | VFS_BACKEND_REG_OVERLAY)

static void vfs_backend_tab_ensure(void)
{
        if (vfs_backend_tab_ready) {
                return;
        }
        if (vfs_slice_table_init(&vfs_backend_tab, sizeof(vfs_backend_entry_t),
                                 8)
            == REND_SUCCESS) {
                vfs_backend_tab.soft_max = VFS_BACKEND_REGISTRY_SOFT_MAX;
                vfs_backend_tab_ready = true;
        }
}

static vfs_backend_entry_t *vfs_backend_at(u32 i)
{
        return (vfs_backend_entry_t *)vfs_slice_table_ptr(&vfs_backend_tab, i);
}

static vfs_backend_entry_t *vfs_backend_find_port(const char *port_name)
{
        u32 i;
        u32 n;

        if (!port_name) {
                return NULL;
        }

        vfs_backend_tab_ensure();
        n = vfs_slice_table_count(&vfs_backend_tab);
        for (i = 0; i < n; i++) {
                vfs_backend_entry_t *e = vfs_backend_at(i);

                if (e && e->active
                    && strcmp_s(e->port_name, port_name, VFS_PATH_MAX) == 0) {
                        return e;
                }
        }

        return NULL;
}

static vfs_backend_entry_t *vfs_backend_alloc_slot(void)
{
        u32 i;
        u32 n;
        u32 idx;
        error_t err;
        vfs_backend_entry_t *e;

        vfs_backend_tab_ensure();
        if (!vfs_backend_tab_ready) {
                return NULL;
        }

        n = vfs_slice_table_count(&vfs_backend_tab);
        for (i = 0; i < n; i++) {
                e = vfs_backend_at(i);

                if (e && !e->active) {
                        memset(e, 0, sizeof(*e));
                        return e;
                }
        }

        err = vfs_slice_table_push_zero(&vfs_backend_tab, &idx);
        if (err != REND_SUCCESS) {
                return NULL;
        }

        e = vfs_backend_at(idx);
        if (!e) {
                vfs_slice_table_pop_last(&vfs_backend_tab);
                return NULL;
        }
        return e;
}

i64 vfs_backend_register(const char *port_name, const char *fstype, u32 caps,
                         u32 reg_flags)
{
        vfs_backend_entry_t *entry;

        if (!port_name || !port_name[0]) {
                return -LINUX_EINVAL;
        }

        entry = vfs_backend_find_port(port_name);
        if (!entry) {
                entry = vfs_backend_alloc_slot();
                if (!entry) {
                        return -LINUX_ENOMEM;
                }
                entry->port_name = port_name;
                entry->active = true;
        }

        if (fstype) {
                strncpy(entry->fstype, fstype, sizeof(entry->fstype) - 1);
                entry->fstype[sizeof(entry->fstype) - 1] = '\0';
        } else {
                entry->fstype[0] = '\0';
        }

        entry->caps = caps;
        entry->reg_flags = reg_flags;

        if (reg_flags & VFS_BACKEND_REG_ROOT) {
                if (vfs_backend_root_port_ptr
                    && strcmp_s(vfs_backend_root_port_ptr, port_name,
                                VFS_PATH_MAX)
                               != 0) {
                        return -LINUX_EEXIST;
                }
                vfs_backend_root_port_ptr = port_name;
        }

        if (reg_flags & VFS_BACKEND_REG_OVERLAY) {
                if (vfs_backend_overlay_port_ptr
                    && strcmp_s(vfs_backend_overlay_port_ptr, port_name,
                                VFS_PATH_MAX)
                               != 0) {
                        return -LINUX_EEXIST;
                }
                vfs_backend_overlay_port_ptr = port_name;
        }

        return 0;
}

u32 vfs_backend_caps_for_port(const char *port_name)
{
        vfs_backend_entry_t *entry = vfs_backend_find_port(port_name);

        return entry ? entry->caps : 0;
}

const char *vfs_backend_port_for_fstype(const char *fstype)
{
        u32 i;
        u32 n;

        if (!fstype || !fstype[0]) {
                return NULL;
        }

        vfs_backend_tab_ensure();
        n = vfs_slice_table_count(&vfs_backend_tab);
        for (i = 0; i < n; i++) {
                vfs_backend_entry_t *e = vfs_backend_at(i);

                if (e && e->active && e->fstype[0]
                    && strcmp_s(e->fstype, fstype, VFS_BACKEND_FSTYPE_MAX)
                               == 0) {
                        return e->port_name;
                }
        }

        return NULL;
}

const char *vfs_backend_root_port(void)
{
        return vfs_backend_root_port_ptr;
}

const char *vfs_backend_overlay_port(void)
{
        return vfs_backend_overlay_port_ptr;
}

const char *vfs_backend_port_for_path(const char *path)
{
        vfs_mount_view_t view;

        if (vfs_mount_view_for_path(path, &view)) {
                return view.backend_port;
        }

        return vfs_backend_overlay_port();
}

bool vfs_backend_port_known(const char *port_name)
{
        return vfs_backend_find_port(port_name) != NULL;
}

void vfs_backend_mark_online(u32 reg_flags)
{
        vfs_backend_online_flags |= reg_flags;
}

bool vfs_backend_boot_io_ready(void)
{
        return (vfs_backend_online_flags & VFS_BACKEND_BOOT_IO_READY)
               == VFS_BACKEND_BOOT_IO_READY;
}

i64 vfs_backend_dispatch(vfs_backend_req_t *req)
{
        if (!req || !req->port) {
                return -LINUX_EINVAL;
        }
        if (!vfs_backend_port_known(req->port)) {
                return -LINUX_ENXIO;
        }

        return vfs_backend_ipc_call(req);
}

bool vfs_backend_lookup(const char *port, const char *path, vfs_inode_t *out)
{
        vfs_backend_req_t req;

        if (!port || !path || !out) {
                return false;
        }

        req.port = port;
        req.op = VFS_BACKEND_OP_LOOKUP;
        req.path = path;
        req.ino = NULL;
        req.ino_out = out;
        req.offset = 0;
        req.len = 0;
        req.buf = NULL;
        req.wbuf = NULL;
        req.size_arg = 0;
        req.result = -LINUX_EINVAL;

        if (vfs_backend_dispatch(&req) != 0) {
                return false;
        }

        return req.result == 0;
}

i64 vfs_backend_readdir(const char *port, const char *dirpath, u64 index,
                        vfs_dirent_t *out)
{
        vfs_backend_req_t req;

        if (!port || !dirpath || !out) {
                return -LINUX_EINVAL;
        }

        req.port = port;
        req.op = VFS_BACKEND_OP_READDIR;
        req.path = dirpath;
        req.dir_index = index;
        req.dirent_out = out;
        req.result = -LINUX_EINVAL;

        if (vfs_backend_dispatch(&req) != 0) {
                return req.result;
        }

        return req.result;
}

i64 vfs_backend_readlink(const char *port, const char *path, char *buf,
                         u64 buf_cap)
{
        vfs_backend_req_t req;

        if (!port || !path || !buf || buf_cap == 0) {
                return -LINUX_EINVAL;
        }

        req.port = port;
        req.op = VFS_BACKEND_OP_READLINK;
        req.path = path;
        req.readlink_buf = buf;
        req.readlink_cap = buf_cap;
        req.result = -LINUX_EINVAL;

        if (vfs_backend_dispatch(&req) != 0) {
                return req.result;
        }

        return req.result;
}

static i64 vfs_backend_path_op(const char *port, vfs_backend_op_t op,
                               const char *path, const char *path2, u32 mode)
{
        vfs_backend_req_t req;

        if (!port || !path) {
                return -LINUX_EINVAL;
        }

        req.port = port;
        req.op = op;
        req.path = path;
        req.path2 = path2;
        req.mode_arg = mode;
        req.result = -LINUX_EINVAL;

        if (vfs_backend_dispatch(&req) != 0) {
                return req.result;
        }

        return req.result;
}

i64 vfs_backend_mkdir(const char *port, const char *path, u32 mode)
{
        return vfs_backend_path_op(port, VFS_BACKEND_OP_MKDIR, path, NULL, mode);
}

i64 vfs_backend_create(const char *port, const char *path, u32 mode)
{
        return vfs_backend_path_op(port, VFS_BACKEND_OP_CREATE, path, NULL, mode);
}

i64 vfs_backend_unlink(const char *port, const char *path)
{
        return vfs_backend_path_op(port, VFS_BACKEND_OP_UNLINK, path, NULL, 0);
}

i64 vfs_backend_rename(const char *port, const char *oldpath, const char *newpath)
{
        if (!oldpath || !newpath) {
                return -LINUX_EINVAL;
        }

        return vfs_backend_path_op(port, VFS_BACKEND_OP_RENAME, oldpath, newpath,
                                   0);
}

i64 vfs_backend_link(const char *port, const char *oldpath, const char *newpath)
{
        if (!oldpath || !newpath) {
                return -LINUX_EINVAL;
        }

        return vfs_backend_path_op(port, VFS_BACKEND_OP_LINK, oldpath, newpath, 0);
}
