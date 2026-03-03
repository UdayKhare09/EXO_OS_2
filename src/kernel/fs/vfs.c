/* fs/vfs.c — VFS core: mount table, path resolution, vnode lifecycle */
#include "vfs.h"
#include "fd.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

#include <stdbool.h>

/* ── Registered filesystem types ─────────────────────────────────────────── */
static fs_ops_t *g_fs_types[VFS_MAX_FS_TYPES];
static int       g_fs_type_count = 0;

/* ── Mount table ─────────────────────────────────────────────────────────── */
static mount_t  g_mounts[VFS_MAX_MOUNTS];
static vnode_t *g_root_vnode = NULL;

/* ── Vnode pool (kmalloc-backed, no fixed pool limit) ─────────────────────── */
vnode_t *vfs_alloc_vnode(void) {
    return kzalloc(sizeof(vnode_t));
}

/* ── Init ─────────────────────────────────────────────────────────────────── */
void vfs_init(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
    g_root_vnode = NULL;
    KLOG_INFO("vfs: initialised\n");
}

/* ── Filesystem registry ─────────────────────────────────────────────────── */
int vfs_register_fs(fs_ops_t *ops) {
    if (g_fs_type_count >= VFS_MAX_FS_TYPES) return -ENOMEM;
    g_fs_types[g_fs_type_count++] = ops;
    KLOG_INFO("vfs: registered filesystem '%s'\n", ops->name);
    return 0;
}

static fs_ops_t *find_fs_type(const char *name) {
    for (int i = 0; i < g_fs_type_count; i++)
        if (strcmp(g_fs_types[i]->name, name) == 0)
            return g_fs_types[i];
    return NULL;
}

/* ── Vnode reference counting ────────────────────────────────────────────── */
void vfs_vnode_get(vnode_t *v) {
    if (v) v->refcount++;
}

void vfs_vnode_put(vnode_t *v) {
    if (!v) return;
    if (v->refcount == 0) return;
    v->refcount--;
    if (v->refcount == 0) {
        if (v->ops && v->ops->evict) v->ops->evict(v);
        /* fs is responsible for freeing v->fs_data; we free the vnode shell */
        kfree(v);
    }
}

/* ── Mount ───────────────────────────────────────────────────────────────── */
int vfs_mount(const char *path, blkdev_t *dev, const char *fstype) {
    fs_ops_t *ops = find_fs_type(fstype);
    if (!ops) {
        KLOG_WARN("vfs: mount: unknown filesystem '%s'\n", fstype);
        return -EINVAL;
    }

    /* Find free mount slot */
    mount_t *m = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].active) { m = &g_mounts[i]; break; }
    }
    if (!m) { KLOG_WARN("vfs: mount table full\n"); return -ENOMEM; }

    /* Allocate fs instance */
    fs_inst_t *fsi = kzalloc(sizeof(fs_inst_t));
    if (!fsi) return -ENOMEM;
    fsi->dev    = dev;
    fsi->ops    = ops;
    fsi->mount  = m;
    fsi->dev_id = (uint64_t)(m - g_mounts) + 1; /* 1-based mount-slot index */

    /* Call filesystem's mount operation */
    vnode_t *root = ops->mount(fsi, dev);
    if (!root) {
        KLOG_WARN("vfs: mount: '%s' failed to mount on '%s'\n",
                  fstype, path ? path : "(null)");
        kfree(fsi);
        return -EIO;
    }
    fsi->root = root;
    root->fsi = fsi;

    /* Fill mount entry */
    strncpy(m->path, path, VFS_MOUNT_PATH_MAX - 1);
    m->root   = root;
    m->fsi    = fsi;
    m->active = true;

    if (strcmp(path, "/") == 0 || g_root_vnode == NULL) {
        g_root_vnode = root;
        m->covered   = NULL;
    } else {
        /* Find the vnode at path to record the covered vnode */
        int err = 0;
        vnode_t *covered = vfs_lookup(path, true, &err);
        m->covered = covered; /* may be NULL if path doesn't exist yet       */
        if (covered) vfs_vnode_put(covered);
    }

    KLOG_INFO("vfs: mounted '%s' on '%s'\n", fstype, path);
    return 0;
}

int vfs_unmount(const char *path) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &g_mounts[i];
        if (!m->active) continue;
        if (strcmp(m->path, path) == 0) {
            if (m->fsi->ops->unmount) m->fsi->ops->unmount(m->fsi);
            kfree(m->fsi);
            m->active = false;
            KLOG_INFO("vfs: unmounted '%s'\n", path);
            if (strcmp(path, "/") == 0) g_root_vnode = NULL;
            return 0;
        }
    }
    return -ENOENT;
}

vnode_t *vfs_get_root(void) {
    return g_root_vnode;
}

/* ── Find the deepest mount that is a prefix of `path` ───────────────────── */
static mount_t *find_mount_for_path(const char *path) {
    mount_t *best = NULL;
    size_t   best_len = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &g_mounts[i];
        if (!m->active) continue;
        size_t mlen = strlen(m->path);
        if (strncmp(path, m->path, mlen) == 0) {
            /* Ensure mount path is a proper prefix (ends at '/' boundary) */
            if (path[mlen] == '/' || path[mlen] == '\0') {
                if (mlen > best_len) { best = m; best_len = mlen; }
            }
        }
    }
    return best;
}

/* ── Check if a resolved path is a mount point; return mounted root if so ── */
static mount_t *find_mount_exact(const char *resolved_path) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &g_mounts[i];
        if (!m->active) continue;
        if (strcmp(m->path, resolved_path) == 0) return m;
    }
    return NULL;
}

static void trim_resolved_parent(char *resolved) {
    if (!resolved || resolved[0] == '\0') return;
    char *last_slash = strrchr(resolved, '/');
    if (last_slash && last_slash != resolved)
        *last_slash = '\0';
    else
        resolved[0] = '\0';
}

/* ── Path resolution ─────────────────────────────────────────────────────── */
#define MAX_SYMLINK_DEPTH 8

vnode_t *vfs_lookup(const char *path, bool follow_last_link, int *err_out) {
    if (!path || path[0] != '/') {
        if (err_out) *err_out = -EINVAL;
        return NULL;
    }
    if (!g_root_vnode) {
        if (err_out) *err_out = -EIO;
        return NULL;
    }

    /* Find the deepest mount that is a prefix of `path` to start from */
    mount_t *start_mount = find_mount_for_path(path);
    vnode_t *cur;
    const char *p;

    if (start_mount && strcmp(start_mount->path, "/") != 0) {
        /* Path starts inside a non-root mount; begin at its root vnode
         * and skip the mount-path prefix in the input path. */
        cur = start_mount->root;
        vfs_vnode_get(cur);
        p = path + strlen(start_mount->path);
        if (*p == '/') p++;  /* skip separator after mount prefix */
    } else {
        cur = g_root_vnode;
        vfs_vnode_get(cur);
        p = path + 1;  /* skip leading '/' */
    }

    int  link_depth = 0;
    char component[VFS_NAME_MAX + 1];
    /* Track the resolved path so we can match mount points by path */
    char resolved[VFS_MOUNT_PATH_MAX];
    if (start_mount && strcmp(start_mount->path, "/") != 0) {
        strncpy(resolved, start_mount->path, VFS_MOUNT_PATH_MAX - 1);
        resolved[VFS_MOUNT_PATH_MAX - 1] = '\0';
    } else {
        resolved[0] = '\0';  /* will become "/" after first append */
    }

    while (*p) {
        /* Skip redundant slashes */
        while (*p == '/') p++;
        if (!*p) break;

        /* Extract next path component */
        int clen = 0;
        while (*p && *p != '/' && clen < VFS_NAME_MAX)
            component[clen++] = *p++;
        component[clen] = '\0';

        bool is_last = (*p == '\0' || (*p == '/' && *(p+1) == '\0'));

        /* Must be a directory to descend */
        if (!VFS_S_ISDIR(cur->mode)) {
            vfs_vnode_put(cur);
            if (err_out) *err_out = -ENOTDIR;
            return NULL;
        }

        /* Handle special names */
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            /* If currently at a mounted root (non-/), cross back to parent mount path. */
            mount_t *at_mount = find_mount_exact(resolved);
            if (at_mount && strcmp(at_mount->path, "/") != 0) {
                trim_resolved_parent(resolved);

                char parent_path[VFS_MOUNT_PATH_MAX];
                if (resolved[0] == '\0') strcpy(parent_path, "/");
                else {
                    strncpy(parent_path, resolved, sizeof(parent_path) - 1);
                    parent_path[sizeof(parent_path) - 1] = '\0';
                }

                int sub_err = 0;
                vnode_t *parent_v = vfs_lookup(parent_path, true, &sub_err);
                if (!parent_v) {
                    vfs_vnode_put(cur);
                    if (err_out) *err_out = sub_err ? sub_err : -ENOENT;
                    return NULL;
                }
                vfs_vnode_put(cur);
                cur = parent_v;
                continue;
            }

            /* Regular in-filesystem parent traversal. */
            if (cur->ops && cur->ops->lookup) {
                vnode_t *parent_v = cur->ops->lookup(cur, "..");
                if (parent_v) {
                    vfs_vnode_put(cur);
                    cur = parent_v;
                }
            }
            trim_resolved_parent(resolved);
            continue;
        }

        /* Build the resolved path for this component */
        size_t rlen = strlen(resolved);
        if (rlen == 0 || resolved[rlen - 1] != '/') {
            if (rlen + 1 < VFS_MOUNT_PATH_MAX) {
                resolved[rlen] = '/';
                resolved[rlen + 1] = '\0';
                rlen++;
            }
        }
        if (rlen + (size_t)clen < VFS_MOUNT_PATH_MAX) {
            memcpy(resolved + rlen, component, clen);
            resolved[rlen + clen] = '\0';
        }

        /* Check if this resolved path is a mount point */
        mount_t *mp = find_mount_exact(resolved);
        if (mp && mp->root && mp->root != cur) {
            /* Switch to the mounted filesystem's root */
            vfs_vnode_put(cur);
            cur = mp->root;
            vfs_vnode_get(cur);
            continue;  /* don't do a lookup in the underlying fs */
        }

        if (!cur->ops || !cur->ops->lookup) {
            vfs_vnode_put(cur);
            if (err_out) *err_out = -EIO;
            return NULL;
        }

        vnode_t *child = cur->ops->lookup(cur, component);
        vfs_vnode_put(cur);
        if (!child) {
            if (err_out) *err_out = -ENOENT;
            return NULL;
        }

        /* Follow symlinks (unless last component and !follow_last_link) */
        if (VFS_S_ISLNK(child->mode) && (!is_last || follow_last_link)) {
            if (link_depth++ >= MAX_SYMLINK_DEPTH) {
                vfs_vnode_put(child);
                if (err_out) *err_out = -ELOOP;
                return NULL;
            }
            char link_target[VFS_MOUNT_PATH_MAX];
            if (!child->ops || !child->ops->readlink ||
                child->ops->readlink(child, link_target, sizeof(link_target)) < 0) {
                vfs_vnode_put(child);
                if (err_out) *err_out = -EIO;
                return NULL;
            }
            vfs_vnode_put(child);

            /* Recurse for symlink target (only absolute links for now) */
            if (link_target[0] == '/') {
                /* Absolute symlink: preserve remaining suffix after this component */
                char combined[VFS_MOUNT_PATH_MAX];
                const char *rest = p;
                while (*rest == '/') rest++;

                if (*rest) {
                    size_t lt_len = strlen(link_target);
                    size_t rest_len = strlen(rest);
                    if (lt_len + 1 + rest_len >= sizeof(combined)) {
                        if (err_out) *err_out = -ENAMETOOLONG;
                        return NULL;
                    }
                    memcpy(combined, link_target, lt_len);
                    if (lt_len == 0 || combined[lt_len - 1] != '/') {
                        combined[lt_len++] = '/';
                    }
                    memcpy(combined + lt_len, rest, rest_len + 1);
                } else {
                    strncpy(combined, link_target, sizeof(combined) - 1);
                    combined[sizeof(combined) - 1] = '\0';
                }

                int sub_err = 0;
                vnode_t *result = vfs_lookup(combined, follow_last_link, &sub_err);
                if (!result && err_out) *err_out = sub_err;
                return result;
            }
            /* Relative symlink — treat as relative to current dir (simplified) */
            continue;
        }

        cur = child;
    }

    if (err_out) *err_out = 0;
    return cur;
}

/* ── Helper: split path into parent + basename ───────────────────────────── */
static int split_path(const char *path, char *parent_out, size_t parent_sz,
                      char *name_out, size_t name_sz) {
    size_t plen = strlen(path);
    if (plen == 0 || path[0] != '/') return -EINVAL;

    /* find last '/' */
    const char *last_slash = path + plen - 1;
    while (last_slash > path && *last_slash != '/') last_slash--;

    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) parent_len = 1; /* keep "/" */

    if (parent_len >= parent_sz) return -ENAMETOOLONG;
    memcpy(parent_out, path, parent_len);
    parent_out[parent_len] = '\0';

    const char *name = last_slash + 1;
    if (strlen(name) >= name_sz) return -ENAMETOOLONG;
    strcpy(name_out, name);
    return 0;
}

file_t *vfs_open(const char *path, int flags, uint32_t mode) {
    if (!path) return NULL;

    int err = 0;
    vnode_t *v = vfs_lookup(path, !(flags & O_NOFOLLOW), &err);

    if (!v) {
        if (!(flags & O_CREAT)) return NULL;

        char parent[VFS_MOUNT_PATH_MAX], name[VFS_NAME_MAX + 1];
        if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
            return NULL;

        vnode_t *pdir = vfs_lookup(parent, true, &err);
        if (!pdir) return NULL;

        if (!pdir->ops || !pdir->ops->create) {
            vfs_vnode_put(pdir);
            return NULL;
        }

        v = pdir->ops->create(pdir, name, mode);
        vfs_vnode_put(pdir);
        if (!v) return NULL;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vfs_vnode_put(v);
        return NULL;
    }

    if ((flags & O_DIRECTORY) && !VFS_S_ISDIR(v->mode)) {
        vfs_vnode_put(v);
        return NULL;
    }

    if ((flags & O_TRUNC) && ((flags & O_ACCMODE) != O_RDONLY)) {
        if (!v->ops || !v->ops->truncate || v->ops->truncate(v, 0) < 0) {
            vfs_vnode_put(v);
            return NULL;
        }
    }

    if (v->ops && v->ops->open) {
        int rc = v->ops->open(v, flags);
        if (rc < 0) {
            vfs_vnode_put(v);
            return NULL;
        }
    }

    file_t *f = file_alloc(v, flags);
    if (!f) {
        if (v->ops && v->ops->close)
            v->ops->close(v);
        vfs_vnode_put(v);
        return NULL;
    }

    strncpy(f->path, path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = '\0';

    /* Transfer any open()-injected f_ops (e.g. PTY master from /dev/ptmx). */
    if (v->pending_f_ops) {
        f->f_ops        = v->pending_f_ops;
        f->private_data = v->pending_priv;
        v->pending_f_ops = NULL;
        v->pending_priv  = NULL;
    }

    vfs_vnode_put(v);
    return f;
}

/* ── VFS operations ──────────────────────────────────────────────────────── */
int vfs_stat(const char *path, vfs_stat_t *st) {
    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (!v) return err ? err : -ENOENT;
    int ret = 0;
    if (v->ops && v->ops->stat) ret = v->ops->stat(v, st);
    else ret = -EIO;
    vfs_vnode_put(v);
    return ret;
}

int vfs_lstat(const char *path, vfs_stat_t *st) {
    int err = 0;
    vnode_t *v = vfs_lookup(path, false, &err);
    if (!v) return err ? err : -ENOENT;
    int ret = 0;
    if (v->ops && v->ops->stat) ret = v->ops->stat(v, st);
    else ret = -EIO;
    vfs_vnode_put(v);
    return ret;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    char parent[VFS_MOUNT_PATH_MAX], name[VFS_NAME_MAX + 1];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return -EINVAL;

    int err = 0;
    vnode_t *pdir = vfs_lookup(parent, true, &err);
    if (!pdir) return err ? err : -ENOENT;

    vnode_t *newdir = NULL;
    if (pdir->ops && pdir->ops->mkdir)
        newdir = pdir->ops->mkdir(pdir, name, mode);
    vfs_vnode_put(pdir);
    if (!newdir) return -EIO;
    vfs_vnode_put(newdir);
    return 0;
}

int vfs_unlink(const char *path) {
    char parent[VFS_MOUNT_PATH_MAX], name[VFS_NAME_MAX + 1];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return -EINVAL;

    int err = 0;
    vnode_t *pdir = vfs_lookup(parent, true, &err);
    if (!pdir) return err ? err : -ENOENT;

    int ret = -EIO;
    if (pdir->ops && pdir->ops->unlink)
        ret = pdir->ops->unlink(pdir, name);
    vfs_vnode_put(pdir);
    return ret;
}

int vfs_rmdir(const char *path) {
    char parent[VFS_MOUNT_PATH_MAX], name[VFS_NAME_MAX + 1];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return -EINVAL;

    int err = 0;
    vnode_t *pdir = vfs_lookup(parent, true, &err);
    if (!pdir) return err ? err : -ENOENT;

    int ret = -EIO;
    if (pdir->ops && pdir->ops->rmdir)
        ret = pdir->ops->rmdir(pdir, name);
    else if (pdir->ops && pdir->ops->unlink)
        ret = pdir->ops->unlink(pdir, name);
    vfs_vnode_put(pdir);
    return ret;
}

int vfs_rename(const char *old_path, const char *new_path) {
    char old_parent[VFS_MOUNT_PATH_MAX], old_name[VFS_NAME_MAX + 1];
    char new_parent[VFS_MOUNT_PATH_MAX], new_name[VFS_NAME_MAX + 1];

    if (split_path(old_path, old_parent, sizeof(old_parent),
                   old_name, sizeof(old_name)) < 0) return -EINVAL;
    if (split_path(new_path, new_parent, sizeof(new_parent),
                   new_name, sizeof(new_name)) < 0) return -EINVAL;

    int err = 0;
    vnode_t *old_dir = vfs_lookup(old_parent, true, &err);
    if (!old_dir) return err ? err : -ENOENT;

    vnode_t *new_dir = vfs_lookup(new_parent, true, &err);
    if (!new_dir) { vfs_vnode_put(old_dir); return err ? err : -ENOENT; }

    int ret = -EIO;
    if (old_dir->ops && old_dir->ops->rename)
        ret = old_dir->ops->rename(old_dir, old_name, new_dir, new_name);

    vfs_vnode_put(old_dir);
    vfs_vnode_put(new_dir);
    return ret;
}

int vfs_symlink(const char *target, const char *link_path) {
    char parent[VFS_MOUNT_PATH_MAX], name[VFS_NAME_MAX + 1];
    if (split_path(link_path, parent, sizeof(parent), name, sizeof(name)) < 0)
        return -EINVAL;

    int err = 0;
    vnode_t *pdir = vfs_lookup(parent, true, &err);
    if (!pdir) return err ? err : -ENOENT;

    int ret = -EIO;
    if (pdir->ops && pdir->ops->symlink) {
        vnode_t *lnk = pdir->ops->symlink(pdir, name, target);
        if (lnk) { vfs_vnode_put(lnk); ret = 0; }
    }
    vfs_vnode_put(pdir);
    return ret;
}

int vfs_readlink(const char *path, char *buf, size_t len) {
    int err = 0;
    vnode_t *v = vfs_lookup(path, false, &err);
    if (!v) return err ? err : -ENOENT;
    int ret = -EINVAL;
    if (VFS_S_ISLNK(v->mode) && v->ops && v->ops->readlink)
        ret = v->ops->readlink(v, buf, len);
    vfs_vnode_put(v);
    return ret;
}

int vfs_truncate(const char *path, uint64_t size) {
    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (!v) return err ? err : -ENOENT;
    int ret = -EINVAL;
    if (v->ops && v->ops->truncate) ret = v->ops->truncate(v, size);
    vfs_vnode_put(v);
    return ret;
}

void vfs_sync_all(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &g_mounts[i];
        if (!m->active) continue;
        if (m->fsi->ops->sync) {
            /* sync root vnode */
            m->fsi->ops->sync(m->root);
        }
    }
}

int vfs_snapshot_mounts(vfs_mount_info_t *buf, int max_count) {
    int total = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &g_mounts[i];
        if (!m->active) continue;

        if (buf && total < max_count) {
            vfs_mount_info_t *out = &buf[total];
            strncpy(out->path, m->path, sizeof(out->path) - 1);
            out->path[sizeof(out->path) - 1] = '\0';

            if (m->fsi && m->fsi->ops && m->fsi->ops->name) {
                strncpy(out->fs_name, m->fsi->ops->name, sizeof(out->fs_name) - 1);
                out->fs_name[sizeof(out->fs_name) - 1] = '\0';
            } else {
                strncpy(out->fs_name, "unknown", sizeof(out->fs_name) - 1);
                out->fs_name[sizeof(out->fs_name) - 1] = '\0';
            }

            if (m->fsi && m->fsi->dev && m->fsi->dev->name[0]) {
                strncpy(out->dev_name, m->fsi->dev->name, sizeof(out->dev_name) - 1);
                out->dev_name[sizeof(out->dev_name) - 1] = '\0';
            } else {
                strncpy(out->dev_name, "none", sizeof(out->dev_name) - 1);
                out->dev_name[sizeof(out->dev_name) - 1] = '\0';
            }
        }
        total++;
    }
    return total;
}
