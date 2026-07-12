#include "vfs_backend.h"

#include "vfs_backend_ipc.h"

#include <common/string.h>
#include <linux_compat/errno.h>

typedef struct vfs_backend_entry {
        const char *port_name;
        char fstype[VFS_BACKEND_FSTYPE_MAX];
        u32 caps;
        u32 reg_flags;
        bool active;
} vfs_backend_entry_t;

static vfs_backend_entry_t vfs_backend_registry[VFS_BACKEND_REGISTRY_MAX];
static const char *vfs_backend_root_port_ptr;
static const char *vfs_backend_overlay_port_ptr;
static u32 vfs_backend_online_flags;

#define VFS_BACKEND_ONLINE_ROOT    VFS_BACKEND_REG_ROOT
#define VFS_BACKEND_ONLINE_OVERLAY VFS_BACKEND_REG_OVERLAY
#define VFS_BACKEND_BOOT_IO_READY  (VFS_BACKEND_ONLINE_ROOT | VFS_BACKEND_ONLINE_OVERLAY)

static vfs_backend_entry_t *vfs_backend_find_port(const char *port_name)
{
        u32 i;

        if (!port_name) {
                return NULL;
        }

        for (i = 0; i < VFS_BACKEND_REGISTRY_MAX; i++) {
                if (vfs_backend_registry[i].active
                    && strcmp_s(vfs_backend_registry[i].port_name, port_name,
                                VFS_PATH_MAX)
                               == 0) {
                        return &vfs_backend_registry[i];
                }
        }

        return NULL;
}

static vfs_backend_entry_t *vfs_backend_alloc_slot(void)
{
        u32 i;

        for (i = 0; i < VFS_BACKEND_REGISTRY_MAX; i++) {
                if (!vfs_backend_registry[i].active) {
                        return &vfs_backend_registry[i];
                }
        }

        return NULL;
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

        if (!fstype || !fstype[0]) {
                return NULL;
        }

        for (i = 0; i < VFS_BACKEND_REGISTRY_MAX; i++) {
                if (vfs_backend_registry[i].active
                    && vfs_backend_registry[i].fstype[0]
                    && strcmp_s(vfs_backend_registry[i].fstype, fstype,
                                VFS_BACKEND_FSTYPE_MAX)
                               == 0) {
                        return vfs_backend_registry[i].port_name;
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
