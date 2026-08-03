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

#include "vfs_slice_table.h"

static vfs_slice_table_t vfs_ns_node_tab;
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
                                 const vfs_ns_node_t *child, bool allow_deleted)
{
        if (!child) {
                return false;
        }
        if (!allow_deleted && child->deleted) {
                return false;
        }
        if (parent && parent->mount_covered && child->in_cpio
            && !child->overlay) {
                return false;
        }
        return true;
}

static vfs_ns_node_t *vfs_ns_alloc_node(vfs_ns_node_t *parent, const char *name,
                                        bool is_dir)
{
        vfs_ns_node_t *node;
        u32 idx;
        error_t err;

        if (!parent || !name || !name[0]) {
                return NULL;
        }

        err = vfs_slice_table_push_zero(&vfs_ns_node_tab, &idx);
        if (err != REND_SUCCESS) {
                return NULL;
        }

        node = (vfs_ns_node_t *)vfs_slice_table_ptr(&vfs_ns_node_tab, idx);
        if (!node) {
                vfs_slice_table_pop_last(&vfs_ns_node_tab);
                return NULL;
        }
        strncpy(node->name, name, sizeof(node->name) - 1);
        node->name[sizeof(node->name) - 1] = '\0';
        node->parent = parent;
        node->is_dir = is_dir;
        node->mode = is_dir ? (0755u | 0040000u) : (0644u | 0100000u);

        return node;
}

#define VFS_NS_PATH_DEPTH_MAX 64u

/* Rebuild absolute path from parent chain (no cached path[] on nodes). */
static bool vfs_ns_path_of(const vfs_ns_node_t *node, char *out, u64 out_cap)
{
        const vfs_ns_node_t *chain[VFS_NS_PATH_DEPTH_MAX];
        u32 depth = 0;
        u32 i;
        char cur[VFS_PATH_MAX];
        char next[VFS_PATH_MAX];

        if (!node || !out || out_cap < 2) {
                return false;
        }

        if (node == &vfs_ns_root) {
                out[0] = '/';
                out[1] = '\0';
                return true;
        }

        while (node && node != &vfs_ns_root) {
                if (depth >= VFS_NS_PATH_DEPTH_MAX) {
                        return false;
                }
                chain[depth++] = node;
                node = node->parent;
        }

        cur[0] = '/';
        cur[1] = '\0';
        for (i = depth; i > 0; i--) {
                if (!vfs_path_join(
                            cur, chain[i - 1]->name, next, sizeof(next))) {
                        return false;
                }
                strncpy(cur, next, sizeof(cur) - 1);
                cur[sizeof(cur) - 1] = '\0';
        }

        strncpy(out, cur, out_cap - 1);
        out[out_cap - 1] = '\0';
        return true;
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

                node = vfs_ns_ensure_child(
                        node, component, last ? is_dir : true);
                if (!node) {
                        return NULL;
                }

                cursor = next;
        }

        return node;
}

static i64 vfs_ns_check_parent_writable(const char *path)
{
        char parent[VFS_PATH_MAX];
        const vfs_ns_node_t *node;

        if (!vfs_path_parent(path, parent, sizeof(parent))) {
                return -LINUX_EINVAL;
        }

        if (vfs_path_is_root(parent)) {
                return vfs_perm_check_mode_request(vfs_ns_root.mode,
                                                   VFS_PERM_W);
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
                pr_error("[VFS][namespace] populate failed (OOM or path): %s\n",
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
        vfs_slice_table_destroy(&vfs_ns_node_tab);
        (void)vfs_slice_table_init(&vfs_ns_node_tab, sizeof(vfs_ns_node_t), 32);
        memset(&vfs_ns_root, 0, sizeof(vfs_ns_root));
        vfs_ns_root.name[0] = '\0';
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

i64 vfs_namespace_lookup_prepare(const char *path, vfs_inode_t *out,
                                 const char **port_out, char *be_path,
                                 u64 be_path_cap)
{
        char norm[VFS_PATH_MAX];
        const vfs_ns_node_t *node;
        vfs_mount_view_t mount_view;
        const char *port;
        char path_buf[VFS_PATH_MAX];

        if (!path || !out || !port_out || !be_path || be_path_cap == 0) {
                return -LINUX_EINVAL;
        }

        *port_out = NULL;
        memset(out, 0, sizeof(*out));
        vfs_path_normalize(path, norm, sizeof(norm));

        if (vfs_path_is_root(norm)) {
                vfs_inode_init_synthetic_root(out);
                return 0;
        }

        if (vfs_mount_view_for_path(norm, &mount_view)) {
                if (strlen(norm) + 1 > be_path_cap) {
                        return -LINUX_ENAMETOOLONG;
                }
                strncpy(be_path, norm, be_path_cap - 1);
                be_path[be_path_cap - 1] = '\0';
                *port_out = mount_view.backend_port;
                return 1;
        }

        node = vfs_ns_lookup_node(norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }

        if (!vfs_ns_path_of(node, path_buf, sizeof(path_buf))) {
                return -LINUX_ENAMETOOLONG;
        }
        if (strlen(path_buf) + 1 > be_path_cap) {
                return -LINUX_ENAMETOOLONG;
        }
        strncpy(be_path, path_buf, be_path_cap - 1);
        be_path[be_path_cap - 1] = '\0';

        if (node->overlay) {
                port = vfs_backend_overlay_port();
                if (!port) {
                        return -LINUX_ENXIO;
                }
                *port_out = port;
                return 1;
        }

        if (node->in_cpio) {
                port = vfs_backend_root_port();
                if (!port) {
                        return -LINUX_ENXIO;
                }
                *port_out = port;
                return 1;
        }

        return vfs_ns_err_noent();
}

i64 vfs_namespace_lookup(const char *path, vfs_inode_t *out)
{
        const char *port = NULL;
        char be_path[VFS_PATH_MAX];
        i64 prep;

        prep = vfs_namespace_lookup_prepare(
                path, out, &port, be_path, sizeof(be_path));
        if (prep <= 0) {
                return prep;
        }
        if (vfs_backend_lookup(port, be_path, out)) {
                return 0;
        }
        return vfs_ns_err_noent();
}

i64 vfs_namespace_mkdir_prepare(const char *path, u32 mode,
                                const char **port_out, char *norm_out,
                                u64 norm_cap, u32 *mode_out,
                                bool *need_commit)
{
        char norm[VFS_PATH_MAX];
        vfs_ns_node_t *node;
        const char *port;
        i64 ret;

        if (!path || !port_out || !norm_out || norm_cap == 0 || !mode_out
            || !need_commit) {
                return -LINUX_EINVAL;
        }

        *port_out = NULL;
        *need_commit = false;
        vfs_path_normalize(path, norm, sizeof(norm));
        if (strlen(norm) + 1 > norm_cap) {
                return -LINUX_ENAMETOOLONG;
        }
        strncpy(norm_out, norm, norm_cap - 1);
        norm_out[norm_cap - 1] = '\0';

        if (vfs_path_is_root(norm)) {
                return vfs_ns_err_exists();
        }

        port = vfs_ns_backend_port(norm);
        if (!port) {
                return -LINUX_ENXIO;
        }

        if (vfs_mount_view_for_path(norm, NULL)) {
                *port_out = port;
                *mode_out = mode;
                *need_commit = false;
                return 1;
        }

        node = vfs_ns_lookup_node(norm, true);
        if (node && !node->deleted) {
                return vfs_ns_err_exists();
        }

        ret = vfs_ns_check_parent_writable(norm);
        if (ret < 0) {
                return ret;
        }

        *port_out = port;
        *mode_out = mode;
        *need_commit = true;
        return 1;
}

i64 vfs_namespace_mkdir_commit(const char *norm, u32 mode)
{
        vfs_ns_node_t *node;
        const char *port;

        if (!norm) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_ensure_path_nodes(norm, true);
        if (!node) {
                port = vfs_ns_backend_port(norm);
                if (port) {
                        (void)vfs_backend_unlink(port, norm);
                }
                return vfs_ns_err_nomem();
        }

        node->deleted = false;
        node->is_dir = true;
        node->in_cpio = false;
        node->overlay = true;
        node->mode = (mode & 0777u) | 0040000u;
        return 0;
}

i64 vfs_namespace_create_prepare(const char *path, u32 mode, vfs_inode_t *out,
                                 const char **port_out, char *norm_out,
                                 u64 norm_cap, u32 *mode_out,
                                 bool *need_commit)
{
        char norm[VFS_PATH_MAX];
        vfs_ns_node_t *node;
        const char *port;
        i64 ret;
        char path_buf[VFS_PATH_MAX];

        if (!path || !port_out || !norm_out || norm_cap == 0 || !mode_out
            || !need_commit) {
                return -LINUX_EINVAL;
        }

        (void)out;
        *port_out = NULL;
        *need_commit = false;
        vfs_path_normalize(path, norm, sizeof(norm));
        if (strlen(norm) + 1 > norm_cap) {
                return -LINUX_ENAMETOOLONG;
        }
        strncpy(norm_out, norm, norm_cap - 1);
        norm_out[norm_cap - 1] = '\0';

        port = vfs_ns_backend_port(norm);
        if (!port) {
                return -LINUX_ENXIO;
        }

        if (vfs_mount_view_for_path(norm, NULL)) {
                *port_out = port;
                *mode_out = mode;
                *need_commit = false;
                return 1;
        }

        node = vfs_ns_lookup_node(norm, true);
        if (node && !node->deleted) {
                if (node->is_dir) {
                        return -LINUX_EISDIR;
                }
                /*
                 * Existing file: caller must LOOKUP (return 2) to check
                 * writable / fill @out — do not block here.
                 */
                if (!vfs_ns_path_of(node, path_buf, sizeof(path_buf))) {
                        return -LINUX_ENAMETOOLONG;
                }
                if (strlen(path_buf) + 1 > norm_cap) {
                        return -LINUX_ENAMETOOLONG;
                }
                strncpy(norm_out, path_buf, norm_cap - 1);
                norm_out[norm_cap - 1] = '\0';
                if (node->overlay) {
                        port = vfs_backend_overlay_port();
                } else if (node->in_cpio) {
                        port = vfs_backend_root_port();
                } else {
                        return vfs_ns_err_noent();
                }
                if (!port) {
                        return -LINUX_ENXIO;
                }
                *port_out = port;
                *mode_out = mode;
                *need_commit = false;
                return 2;
        }

        ret = vfs_ns_check_parent_writable(norm);
        if (ret < 0) {
                return ret;
        }

        *port_out = port;
        *mode_out = mode;
        *need_commit = true;
        return 1;
}

i64 vfs_namespace_create_commit(const char *norm, u32 mode, vfs_inode_t *out)
{
        vfs_ns_node_t *node;
        const char *port;

        (void)out;
        if (!norm) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_ensure_path_nodes(norm, false);
        if (!node) {
                port = vfs_ns_backend_port(norm);
                if (port) {
                        (void)vfs_backend_unlink(port, norm);
                }
                return vfs_ns_err_nomem();
        }

        node->deleted = false;
        node->is_dir = false;
        node->in_cpio = false;
        node->overlay = true;
        node->mode = (mode & 0777u) | 0100000u;
        return 0;
}

i64 vfs_namespace_unlink_prepare(const char *path, const char **port_out,
                                 char *norm_out, u64 norm_cap,
                                 bool *need_backend, bool *need_commit)
{
        char norm[VFS_PATH_MAX];
        vfs_ns_node_t *node;
        const char *port;
        i64 ret;

        if (!path || !port_out || !norm_out || norm_cap == 0 || !need_backend
            || !need_commit) {
                return -LINUX_EINVAL;
        }

        *port_out = NULL;
        *need_backend = false;
        *need_commit = false;
        vfs_path_normalize(path, norm, sizeof(norm));
        if (strlen(norm) + 1 > norm_cap) {
                return -LINUX_ENAMETOOLONG;
        }
        strncpy(norm_out, norm, norm_cap - 1);
        norm_out[norm_cap - 1] = '\0';

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
                *port_out = port;
                *need_backend = true;
                *need_commit = true;
                return 1;
        }

        /* Catalog-only delete: mark deleted locally, no backend. */
        *need_backend = false;
        *need_commit = true;
        return 0;
}

i64 vfs_namespace_unlink_commit(const char *norm)
{
        vfs_ns_node_t *node;

        if (!norm) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_lookup_node(norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }

        if (node->overlay) {
                node->overlay = false;
        }

        vfs_page_cache_drop(norm);
        node->deleted = true;
        return 0;
}

i64 vfs_namespace_readdir_prepare(const char *dirpath, u64 index,
                                  vfs_dirent_t *out, const char **port_out,
                                  char *be_path, u64 be_path_cap)
{
        char norm[VFS_PATH_MAX];
        char path_buf[VFS_PATH_MAX];
        const vfs_ns_node_t *dir;
        const vfs_ns_node_t *child;
        vfs_mount_view_t mount_view;
        u64 i;

        if (!dirpath || !out || !port_out || !be_path || be_path_cap == 0) {
                return -LINUX_EINVAL;
        }

        *port_out = NULL;
        vfs_path_normalize(dirpath, norm, sizeof(norm));

        if (vfs_mount_view_for_path(norm, &mount_view)) {
                if (strlen(norm) + 1 > be_path_cap) {
                        return -LINUX_ENAMETOOLONG;
                }
                strncpy(be_path, norm, be_path_cap - 1);
                be_path[be_path_cap - 1] = '\0';
                *port_out = mount_view.backend_port;
                return 2;
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
                if (!vfs_ns_path_of(dir, path_buf, sizeof(path_buf))) {
                        return -LINUX_ENAMETOOLONG;
                }
                out->d_ino = vfs_path_to_ino(path_buf);
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
                        out->d_type = child->is_symlink ? VFS_DT_LNK :
                                      child->is_dir     ? VFS_DT_DIR :
                                                          VFS_DT_REG;
                        if (!vfs_ns_path_of(child, path_buf, sizeof(path_buf))) {
                                return -LINUX_ENAMETOOLONG;
                        }
                        out->d_ino = vfs_path_to_ino(path_buf);
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

i64 vfs_namespace_rename_prepare(const char *oldpath, const char *newpath,
                                 const char **port_out, char *old_out,
                                 u64 old_cap, char *new_out, u64 new_cap,
                                 bool *need_commit)
{
        char old_norm[VFS_PATH_MAX];
        char new_norm[VFS_PATH_MAX];
        char new_name[64];
        vfs_ns_node_t *node;
        vfs_ns_node_t *dest_parent;
        vfs_ns_node_t *existing;
        const char *port;
        i64 perm_err;

        if (!oldpath || !newpath || !port_out || !old_out || old_cap == 0
            || !new_out || new_cap == 0 || !need_commit) {
                return -LINUX_EINVAL;
        }

        *port_out = NULL;
        *need_commit = false;
        vfs_path_normalize(oldpath, old_norm, sizeof(old_norm));
        vfs_path_normalize(newpath, new_norm, sizeof(new_norm));

        if (strlen(old_norm) + 1 > old_cap || strlen(new_norm) + 1 > new_cap) {
                return -LINUX_ENAMETOOLONG;
        }
        strncpy(old_out, old_norm, old_cap - 1);
        old_out[old_cap - 1] = '\0';
        strncpy(new_out, new_norm, new_cap - 1);
        new_out[new_cap - 1] = '\0';

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

                if (!vfs_path_parent(
                            new_norm, parent_path, sizeof(parent_path))) {
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
                *port_out = port;
                *need_commit = true;
                return 1;
        }
        if (node->in_cpio) {
                return -LINUX_EROFS;
        }
        return vfs_ns_err_noent();
}

i64 vfs_namespace_rename_commit(const char *old_norm, const char *new_norm)
{
        char new_name[64];
        vfs_ns_node_t *node;
        vfs_ns_node_t *dest_parent;
        char parent_path[VFS_PATH_MAX];

        if (!old_norm || !new_norm) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_lookup_node(old_norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }
        if (!vfs_path_basename(new_norm, new_name, sizeof(new_name))) {
                return -LINUX_EINVAL;
        }
        if (!vfs_path_parent(new_norm, parent_path, sizeof(parent_path))) {
                return -LINUX_EINVAL;
        }
        dest_parent = vfs_ns_lookup_node(parent_path, false);
        if (!dest_parent || !dest_parent->is_dir) {
                return vfs_ns_err_noent();
        }

        vfs_page_cache_drop(old_norm);
        vfs_page_cache_drop(new_norm);

        vfs_ns_detach(node);
        strncpy(node->name, new_name, sizeof(node->name) - 1);
        node->name[sizeof(node->name) - 1] = '\0';
        node->parent = dest_parent;
        vfs_ns_link_child(dest_parent, node);
        return 0;
}

i64 vfs_namespace_link_prepare(const char *oldpath, const char *newpath,
                               const char **port_out, char *old_out,
                               u64 old_cap, char *new_out, u64 new_cap,
                               bool *need_commit)
{
        char old_norm[VFS_PATH_MAX];
        char new_norm[VFS_PATH_MAX];
        char new_name[64];
        vfs_ns_node_t *node;
        vfs_ns_node_t *dest_parent;
        vfs_ns_node_t *existing;
        const char *port;
        i64 perm_err;

        if (!oldpath || !newpath || !port_out || !old_out || old_cap == 0
            || !new_out || new_cap == 0 || !need_commit) {
                return -LINUX_EINVAL;
        }

        *port_out = NULL;
        *need_commit = false;
        vfs_path_normalize(oldpath, old_norm, sizeof(old_norm));
        vfs_path_normalize(newpath, new_norm, sizeof(new_norm));

        if (strlen(old_norm) + 1 > old_cap || strlen(new_norm) + 1 > new_cap) {
                return -LINUX_ENAMETOOLONG;
        }
        strncpy(old_out, old_norm, old_cap - 1);
        old_out[old_cap - 1] = '\0';
        strncpy(new_out, new_norm, new_cap - 1);
        new_out[new_cap - 1] = '\0';

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

                if (!vfs_path_parent(
                            new_norm, parent_path, sizeof(parent_path))) {
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

        *port_out = port;
        *need_commit = true;
        return 1;
}

i64 vfs_namespace_link_commit(const char *old_norm, const char *new_norm)
{
        char new_name[64];
        char parent_path[VFS_PATH_MAX];
        vfs_ns_node_t *node;
        vfs_ns_node_t *dest_parent;
        vfs_ns_node_t *link_node;

        if (!old_norm || !new_norm) {
                return -LINUX_EINVAL;
        }

        node = vfs_ns_lookup_node(old_norm, false);
        if (!node) {
                return vfs_ns_err_noent();
        }
        if (!vfs_path_basename(new_norm, new_name, sizeof(new_name))) {
                return -LINUX_EINVAL;
        }
        if (!vfs_path_parent(new_norm, parent_path, sizeof(parent_path))) {
                return -LINUX_EINVAL;
        }
        dest_parent = vfs_ns_lookup_node(parent_path, false);
        if (!dest_parent || !dest_parent->is_dir) {
                return vfs_ns_err_noent();
        }

        link_node = vfs_ns_alloc_node(dest_parent, new_name, false);
        if (!link_node) {
                const char *port = vfs_ns_backend_port(old_norm);

                /* Rare: backend already linked; sync rollback only. */
                if (port) {
                        (void)vfs_backend_unlink(port, new_norm);
                }
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
