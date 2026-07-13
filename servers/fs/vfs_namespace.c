/*
 * Tree-structured VFS namespace: cpio (read-only catalog) + ramfs storage.
 */

#include "vfs_backend.h"
#include "vfs_namespace.h"

#include "cpio_rofs.h"
#include "vfs_mount.h"
#include "vfs_page_cache.h"
#include "vfs_perm.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>

static vfs_ns_node_t vfs_ns_nodes[VFS_NS_MAX_NODES];
static u32 vfs_ns_node_count;
static vfs_ns_node_t vfs_ns_root;
static bool vfs_ns_initialized;

static i64 vfs_ns_err_exists(void)
{
        return -LINUX_EEXIST;
}

static i64 vfs_ns_err_noent(void)
{
        return -LINUX_ENOENT;
}

static i64 vfs_ns_err_nomem(void)
{
        return -LINUX_ENOMEM;
}

static i32 vfs_ns_name_cmp(const char *a, const char *b)
{
        return (i32)strcmp_s(a, b, 64);
}

static bool vfs_ns_child_visible(const vfs_ns_node_t *parent,
                                 const vfs_ns_node_t *child,
                                 bool allow_deleted)
{
        if (!child) {
                return false;
        }
        if (!allow_deleted && child->deleted) {
                return false;
        }
        if (parent && parent->mount_covered && child->in_cpio && !child->overlay) {
                return false;
        }
        return true;
}

static vfs_ns_node_t *vfs_ns_alloc_node(vfs_ns_node_t *parent, const char *name,
                                        bool is_dir)
{
        vfs_ns_node_t *node;
        u64 plen;

        if (!parent || !name || !name[0] || vfs_ns_node_count >= VFS_NS_MAX_NODES) {
                return NULL;
        }

        node = &vfs_ns_nodes[vfs_ns_node_count++];
        memset(node, 0, sizeof(*node));
        strncpy(node->name, name, sizeof(node->name) - 1);
        node->name[sizeof(node->name) - 1] = '\0';
        node->parent = parent;
        node->is_dir = is_dir;
        node->mode = is_dir ? (0755u | 0040000u) : (0644u | 0100000u);

        plen = strlen(parent->path);
        if (parent == &vfs_ns_root || plen <= 1) {
                node->path[0] = '/';
                strncpy(node->path + 1, name, sizeof(node->path) - 2);
                node->path[sizeof(node->path) - 1] = '\0';
        } else {
                if (!vfs_path_join(parent->path, name, node->path,
                                   sizeof(node->path))) {
                        vfs_ns_node_count--;
                        return NULL;
                }
        }

        return node;
}

static void vfs_ns_link_child(vfs_ns_node_t *parent, vfs_ns_node_t *child)
{
        vfs_ns_node_t **cursor;

        if (!parent || !child) {
                return;
        }

        for (cursor = &parent->first_child; *cursor;
             cursor = &(*cursor)->next_sibling) {
                i32 cmp = vfs_ns_name_cmp((*cursor)->name, child->name);

                if (cmp == 0) {
                        return;
                }
                if (cmp > 0) {
                        break;
                }
        }

        child->next_sibling = *cursor;
        *cursor = child;
}

static vfs_ns_node_t *vfs_ns_find_child(const vfs_ns_node_t *parent,
                                        const char *name, bool allow_deleted)
{
        vfs_ns_node_t *child;

        if (!parent || !name) {
                return NULL;
        }

        for (child = parent->first_child; child; child = child->next_sibling) {
                if (vfs_ns_name_cmp(child->name, name) != 0) {
                        continue;
                }
                if (!vfs_ns_child_visible(parent, child, allow_deleted)) {
                        return NULL;
                }
                return child;
        }

        return NULL;
}

static vfs_ns_node_t *vfs_ns_lookup_node(const char *path, bool allow_deleted)
{
        char norm[VFS_PATH_MAX];
        char component[64];
        const char *cursor;
        vfs_ns_node_t *node;

        if (!path) {
                return NULL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        if (vfs_path_is_root(norm)) {
                return &vfs_ns_root;
        }

        node = &vfs_ns_root;
        cursor = norm + 1;

        while (*cursor != '\0') {
                u64 i = 0;
                vfs_ns_node_t *next;

                while (cursor[i] != '\0' && cursor[i] != '/'
                       && i + 1 < sizeof(component)) {
                        component[i] = cursor[i];
                        i++;
                }
                component[i] = '\0';
                if (component[0] == '\0') {
                        return NULL;
                }

                next = vfs_ns_find_child(node, component, allow_deleted);
                if (!next) {
                        return NULL;
                }
                node = next;

                cursor += i;
                if (*cursor == '/') {
                        cursor++;
                }
        }

        return node;
}

static vfs_ns_node_t *vfs_ns_ensure_child(vfs_ns_node_t *parent,
                                          const char *name, bool is_dir)
{
        vfs_ns_node_t *child;

        child = vfs_ns_find_child(parent, name, true);
        if (child) {
                if (child->deleted) {
                        child->deleted = false;
                }
                if (is_dir) {
                        child->is_dir = true;
                }
                return child;
        }

        child = vfs_ns_alloc_node(parent, name, is_dir);
        if (!child) {
                return NULL;
        }

        vfs_ns_link_child(parent, child);
        return child;
}

static vfs_ns_node_t *vfs_ns_ensure_path_nodes(const char *path, bool is_dir)
{
        char norm[VFS_PATH_MAX];
        char component[64];
        const char *cursor;
        vfs_ns_node_t *node;

        vfs_path_normalize(path, norm, sizeof(norm));
        if (vfs_path_is_root(norm)) {
                return &vfs_ns_root;
        }

        node = &vfs_ns_root;
        cursor = norm + 1;

        while (*cursor != '\0') {
                u64 i = 0;
                bool last;
                const char *next;

                while (cursor[i] != '\0' && cursor[i] != '/'
                       && i + 1 < sizeof(component)) {
                        component[i] = cursor[i];
                        i++;
                }
                component[i] = '\0';

                next = cursor + i;
                last = (*next == '\0');
                if (*next == '/') {
                        next++;
                }

                node = vfs_ns_ensure_child(node, component, last ? is_dir : true);
                if (!node) {
                        return NULL;
                }

                cursor = next;
        }

        return node;
}

static i64 vfs_ns_fill_inode(const vfs_ns_node_t *node, vfs_inode_t *out)
{
        const char *port;

        if (!node || !out) {
                return -LINUX_EINVAL;
        }

        /*
         * Overlay vs root catalog (mount paths resolve in vfs_namespace_lookup):
         * 1. Writable overlay node → overlay backend.
         * 2. Cpio catalog node → root backend (boot image).
         */
        if (node->overlay) {
                port = vfs_backend_overlay_port();
                if (!port) {
                        return -LINUX_ENXIO;
                }
                if (vfs_backend_lookup(port, node->path, out)) {
                        return 0;
                }
                return -LINUX_EIO;
        }

        if (node->in_cpio) {
                port = vfs_backend_root_port();
                if (!port) {
                        return -LINUX_ENXIO;
                }
                if (vfs_backend_lookup(port, node->path, out)) {
                        return 0;
                }
                return -LINUX_EIO;
        }

        return vfs_ns_err_noent();
}

static i64 vfs_ns_check_parent_writable(const char *path)
{
        char parent[VFS_PATH_MAX];
        const vfs_ns_node_t *node;

        if (!vfs_path_parent(path, parent, sizeof(parent))) {
                return -LINUX_EINVAL;
        }

        if (vfs_path_is_root(parent)) {
                return vfs_perm_check_mode_request(vfs_ns_root.mode, VFS_PERM_W);
        }

        node = vfs_ns_lookup_node(parent, false);
        if (!node || !node->is_dir) {
                return vfs_ns_err_noent();
        }

        return vfs_perm_check_mode_request(node->mode, VFS_PERM_W);
}

static const char *vfs_ns_backend_port(const char *path)
{
        const char *port = vfs_backend_port_for_path(path);

        if (!port || !port[0]) {
                return NULL;
        }

        return port;
}

static bool vfs_ns_populate_failed;

static bool vfs_ns_populate_cb(const char *path, const cpio_rofs_stat_t *st,
                               void *ctx)
{
        vfs_ns_node_t *node;

        (void)ctx;

        if (!path || !st) {
                return true;
        }

        if (vfs_path_is_root(path)) {
                return true;
        }

        node = vfs_ns_ensure_path_nodes(path, st->is_dir);
        if (!node) {
                pr_error("[VFS][namespace] populate failed (node cap %u): %s\n",
                         VFS_NS_MAX_NODES,
                         path);
                vfs_ns_populate_failed = true;
                return false;
        }

        node->in_cpio = true;
        node->is_symlink = st->is_symlink;
        node->is_dir = st->is_dir;
        node->mode = st->mode ? st->mode :
                                (st->is_dir ? (0755u | 0040000u) :
                                              (0644u | 0100000u));
        node->deleted = false;
        return true;
}

static u32 vfs_ns_count_live_nodes(const vfs_ns_node_t *node)
{
        u32 count = 0;
        vfs_ns_node_t *child;

        if (!node) {
                return 0;
        }

        if (node != &vfs_ns_root && !node->deleted) {
                count = 1;
        }

        for (child = node->first_child; child; child = child->next_sibling) {
                count += vfs_ns_count_live_nodes(child);
        }

        return count;
}

void vfs_namespace_reset(void)
{
        vfs_ns_node_count = 0;
        memset(vfs_ns_nodes, 0, sizeof(vfs_ns_nodes));
        memset(&vfs_ns_root, 0, sizeof(vfs_ns_root));
        vfs_ns_root.path[0] = '/';
        vfs_ns_root.path[1] = '\0';
        vfs_ns_root.is_dir = true;
        vfs_ns_root.mode = 0755u | 0040000u;
        vfs_ns_initialized = false;
        vfs_mount_reset();
        vfs_page_cache_reset();
}

error_t vfs_namespace_init(void)
{
        if (vfs_ns_initialized) {
                return REND_SUCCESS;
        }

        vfs_namespace_reset();
        vfs_ns_populate_failed = false;
        cpio_rofs_visit(vfs_ns_populate_cb, NULL);
        if (vfs_ns_populate_failed) {
                return -E_RENDEZVOS;
        }
        vfs_ns_initialized = true;
        return REND_SUCCESS;
}

u32 vfs_namespace_count(void)
{
        return vfs_ns_count_live_nodes(&vfs_ns_root);
}

i64 vfs_namespace_lookup(const char *path, vfs_inode_t *out)
{
        char norm[VFS_PATH_MAX];
        const vfs_ns_node_t *node;
        vfs_mount_view_t mount_view;

        if (!path || !out) {
                return -LINUX_EINVAL;
        }

        memset(out, 0, sizeof(*out));
        vfs_path_normalize(path, norm, sizeof(norm));

        if (vfs_path_is_root(norm)) {
                vfs_inode_init_synthetic_root(out);
                return 0;
        }

        if (vfs_mount_view_for_path(norm, &mount_view)) {
                if (vfs_backend_lookup(mount_view.backend_port, norm, out)) {
                        return 0;
                }
                return vfs_ns_err_noent();
        }

        node = vfs_ns_lookup_node(norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }

        return vfs_ns_fill_inode(node, out);
}

i64 vfs_namespace_mkdir(const char *path, u32 mode)
{
        char norm[VFS_PATH_MAX];
        vfs_ns_node_t *node;
        const char *port;
        i64 ret;

        if (!path) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        if (vfs_path_is_root(norm)) {
                return vfs_ns_err_exists();
        }

        port = vfs_ns_backend_port(norm);
        if (!port) {
                return -LINUX_ENXIO;
        }

        if (vfs_mount_view_for_path(norm, NULL)) {
                return vfs_backend_mkdir(port, norm, mode);
        }

        node = vfs_ns_lookup_node(norm, true);
        if (node && !node->deleted) {
                return vfs_ns_err_exists();
        }

        ret = vfs_ns_check_parent_writable(norm);
        if (ret < 0) {
                return ret;
        }

        ret = vfs_backend_mkdir(port, norm, mode);
        if (ret < 0) {
                return ret;
        }

        node = vfs_ns_ensure_path_nodes(norm, true);
        if (!node) {
                (void)vfs_backend_unlink(port, norm);
                return vfs_ns_err_nomem();
        }

        node->deleted = false;
        node->is_dir = true;
        node->in_cpio = false;
        node->overlay = true;
        node->mode = (mode & 0777u) | 0040000u;
        return 0;
}

i64 vfs_namespace_create_file(const char *path, u32 mode, vfs_inode_t *out)
{
        char norm[VFS_PATH_MAX];
        vfs_ns_node_t *node;
        const char *port;
        i64 ret;
        vfs_inode_t existing;

        if (!path) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));

        port = vfs_ns_backend_port(norm);
        if (!port) {
                return -LINUX_ENXIO;
        }

        if (vfs_mount_view_for_path(norm, NULL)) {
                ret = vfs_backend_create(port, norm, mode);
                if (ret < 0) {
                        return ret;
                }
                if (out && vfs_namespace_lookup(norm, out) < 0) {
                        return -LINUX_EIO;
                }
                return 0;
        }

        node = vfs_ns_lookup_node(norm, true);
        if (node && !node->deleted) {
                if (node->is_dir) {
                        return -LINUX_EISDIR;
                }
                if (vfs_ns_fill_inode(node, &existing) == 0) {
                        if (!existing.writable) {
                                return vfs_ns_err_exists();
                        }
                        if (out) {
                                *out = existing;
                        }
                        return 0;
                }
                return -LINUX_EIO;
        }

        ret = vfs_ns_check_parent_writable(norm);
        if (ret < 0) {
                return ret;
        }

        ret = vfs_backend_create(port, norm, mode);
        if (ret < 0) {
                return ret;
        }

        node = vfs_ns_ensure_path_nodes(norm, false);
        if (!node) {
                (void)vfs_backend_unlink(port, norm);
                return vfs_ns_err_nomem();
        }

        node->deleted = false;
        node->is_dir = false;
        node->in_cpio = false;
        node->overlay = true;
        node->mode = (mode & 0777u) | 0100000u;

        if (out && vfs_ns_fill_inode(node, out) < 0) {
                return -LINUX_EIO;
        }

        return 0;
}

i64 vfs_namespace_unlink(const char *path)
{
        char norm[VFS_PATH_MAX];
        vfs_ns_node_t *node;
        const char *port;
        i64 ret;

        if (!path) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        if (vfs_path_is_root(norm)) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_lookup_node(norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }

        if (node->is_dir) {
                return -LINUX_EISDIR;
        }

        ret = vfs_perm_check_mode_request(node->mode, VFS_PERM_W);
        if (ret < 0) {
                return ret;
        }

        if (node->overlay) {
                port = vfs_ns_backend_port(norm);
                if (!port) {
                        return -LINUX_ENXIO;
                }
                ret = vfs_backend_unlink(port, norm);
                if (ret < 0) {
                        return ret;
                }
                node->overlay = false;
        }

        vfs_page_cache_drop(norm);

        if (node->in_cpio) {
                node->deleted = true;
                return 0;
        }

        node->deleted = true;
        return 0;
}

i64 vfs_namespace_readdir(const char *dirpath, u64 index, vfs_dirent_t *out)
{
        char norm[VFS_PATH_MAX];
        const vfs_ns_node_t *dir;
        const vfs_ns_node_t *child;
        vfs_mount_view_t mount_view;
        u64 i;

        if (!dirpath || !out) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(dirpath, norm, sizeof(norm));

        if (vfs_mount_view_for_path(norm, &mount_view)) {
                return vfs_backend_readdir(mount_view.backend_port, norm, index,
                                           out);
        }

        dir = vfs_ns_lookup_node(norm, false);
        if (!dir) {
                return vfs_ns_err_noent();
        }
        if (!dir->is_dir) {
                return -LINUX_ENOTDIR;
        }

        if (index == 0) {
                memset(out, 0, sizeof(*out));
                strncpy(out->name, ".", sizeof(out->name) - 1);
                out->d_type = VFS_DT_DIR;
                out->d_ino = vfs_path_to_ino(dir->path);
                return 0;
        }
        if (index == 1) {
                char parent[VFS_PATH_MAX];

                memset(out, 0, sizeof(*out));
                strncpy(out->name, "..", sizeof(out->name) - 1);
                out->d_type = VFS_DT_DIR;
                if (vfs_path_parent(norm, parent, sizeof(parent))) {
                        out->d_ino = vfs_path_to_ino(parent);
                } else {
                        out->d_ino = vfs_path_to_ino("/");
                }
                return 0;
        }

        index -= 2;

        i = 0;
        for (child = dir->first_child; child; child = child->next_sibling) {
                if (!vfs_ns_child_visible(dir, child, false)) {
                        continue;
                }

                if (i == index) {
                        memset(out, 0, sizeof(*out));
                        strncpy(out->name, child->name, sizeof(out->name) - 1);
                        out->name[sizeof(out->name) - 1] = '\0';
                        out->d_type = child->is_symlink ?
                                              VFS_DT_LNK :
                                      child->is_dir ? VFS_DT_DIR : VFS_DT_REG;
                        out->d_ino = vfs_path_to_ino(child->path);
                        return 0;
                }

                i++;
        }

        return 1;
}

static void vfs_ns_detach(vfs_ns_node_t *node)
{
        vfs_ns_node_t *parent;
        vfs_ns_node_t **cursor;

        if (!node || !node->parent) {
                return;
        }

        parent = node->parent;
        for (cursor = &parent->first_child; *cursor;
             cursor = &(*cursor)->next_sibling) {
                if (*cursor == node) {
                        *cursor = node->next_sibling;
                        node->next_sibling = NULL;
                        node->parent = NULL;
                        return;
                }
        }
}

static void vfs_ns_rebuild_paths(vfs_ns_node_t *node)
{
        vfs_ns_node_t *child;

        if (!node) {
                return;
        }

        if (node == &vfs_ns_root) {
                node->path[0] = '/';
                node->path[1] = '\0';
        } else if (node->parent) {
                if (node->parent == &vfs_ns_root) {
                        node->path[0] = '/';
                        strncpy(node->path + 1, node->name,
                                sizeof(node->path) - 2);
                        node->path[sizeof(node->path) - 1] = '\0';
                } else if (!vfs_path_join(node->parent->path, node->name,
                                          node->path, sizeof(node->path))) {
                        return;
                }
        }

        for (child = node->first_child; child; child = child->next_sibling) {
                vfs_ns_rebuild_paths(child);
        }
}

i64 vfs_namespace_set_mount_cover(const char *target, bool covered)
{
        char norm[VFS_PATH_MAX];
        vfs_ns_node_t *node;

        if (!target) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(target, norm, sizeof(norm));
        node = vfs_ns_lookup_node(norm, true);
        if (!node || !node->is_dir) {
                return -LINUX_ENOTDIR;
        }

        node->mount_covered = covered;
        return 0;
}

i64 vfs_namespace_rename(const char *oldpath, const char *newpath)
{
        char old_norm[VFS_PATH_MAX];
        char new_norm[VFS_PATH_MAX];
        char new_name[64];
        vfs_ns_node_t *node;
        vfs_ns_node_t *dest_parent;
        vfs_ns_node_t *existing;
        const char *port;
        i64 perm_err;
        i64 ret;

        if (!oldpath || !newpath) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(oldpath, old_norm, sizeof(old_norm));
        vfs_path_normalize(newpath, new_norm, sizeof(new_norm));

        if (vfs_path_is_root(old_norm) || vfs_path_is_root(new_norm)) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_lookup_node(old_norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }

        existing = vfs_ns_lookup_node(new_norm, false);
        if (existing) {
                return vfs_ns_err_exists();
        }

        perm_err = vfs_ns_check_parent_writable(new_norm);
        if (perm_err < 0) {
                return perm_err;
        }

        if (!vfs_path_basename(new_norm, new_name, sizeof(new_name))) {
                return -LINUX_EINVAL;
        }

        {
                char parent_path[VFS_PATH_MAX];

                if (!vfs_path_parent(new_norm, parent_path, sizeof(parent_path))) {
                        return -LINUX_EINVAL;
                }
                dest_parent = vfs_ns_lookup_node(parent_path, false);
        }
        if (!dest_parent || !dest_parent->is_dir) {
                return vfs_ns_err_noent();
        }

        if (node->overlay) {
                port = vfs_ns_backend_port(old_norm);
                if (!port) {
                        return -LINUX_ENXIO;
                }
                ret = vfs_backend_rename(port, old_norm, new_norm);
                if (ret < 0) {
                        return ret;
                }
        } else if (node->in_cpio) {
                return -LINUX_EROFS;
        } else {
                return vfs_ns_err_noent();
        }

        vfs_page_cache_drop(old_norm);
        vfs_page_cache_drop(new_norm);

        vfs_ns_detach(node);
        strncpy(node->name, new_name, sizeof(node->name) - 1);
        node->name[sizeof(node->name) - 1] = '\0';
        node->parent = dest_parent;
        vfs_ns_link_child(dest_parent, node);
        vfs_ns_rebuild_paths(node);
        return 0;
}

i64 vfs_namespace_link(const char *oldpath, const char *newpath)
{
        char old_norm[VFS_PATH_MAX];
        char new_norm[VFS_PATH_MAX];
        char new_name[64];
        vfs_ns_node_t *node;
        vfs_ns_node_t *dest_parent;
        vfs_ns_node_t *existing;
        vfs_ns_node_t *link_node;
        const char *port;
        i64 perm_err;
        i64 ret;

        if (!oldpath || !newpath) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(oldpath, old_norm, sizeof(old_norm));
        vfs_path_normalize(newpath, new_norm, sizeof(new_norm));

        if (vfs_path_is_root(old_norm) || vfs_path_is_root(new_norm)) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_lookup_node(old_norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }
        if (node->is_dir) {
                return -LINUX_EPERM;
        }
        if (node->in_cpio && !node->overlay) {
                return -LINUX_EXDEV;
        }
        if (!node->overlay) {
                return -LINUX_ENOENT;
        }

        existing = vfs_ns_lookup_node(new_norm, false);
        if (existing) {
                return vfs_ns_err_exists();
        }

        perm_err = vfs_ns_check_parent_writable(new_norm);
        if (perm_err < 0) {
                return perm_err;
        }

        if (!vfs_path_basename(new_norm, new_name, sizeof(new_name))) {
                return -LINUX_EINVAL;
        }

        {
                char parent_path[VFS_PATH_MAX];

                if (!vfs_path_parent(new_norm, parent_path, sizeof(parent_path))) {
                        return -LINUX_EINVAL;
                }
                dest_parent = vfs_ns_lookup_node(parent_path, false);
        }
        if (!dest_parent || !dest_parent->is_dir) {
                return vfs_ns_err_noent();
        }

        port = vfs_ns_backend_port(old_norm);
        if (!port) {
                return -LINUX_ENXIO;
        }

        ret = vfs_backend_link(port, old_norm, new_norm);
        if (ret < 0) {
                return ret;
        }

        link_node = vfs_ns_alloc_node(dest_parent, new_name, false);
        if (!link_node) {
                (void)vfs_backend_unlink(port, new_norm);
                return vfs_ns_err_nomem();
        }

        link_node->deleted = false;
        link_node->is_dir = false;
        link_node->in_cpio = node->in_cpio;
        link_node->overlay = true;
        link_node->mode = node->mode;
        vfs_ns_link_child(dest_parent, link_node);
        return 0;
}
