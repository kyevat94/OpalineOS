/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/cred.h>
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/sched.h>
#include <opaline/string.h>
#include <opaline/uvm.h>
#include <opaline/vfs.h>

static struct vnode *root;

/* ---- references --------------------------------------------------------------- */

void vnode_ref(struct vnode *v) { __atomic_add_fetch(&v->refs, 1, __ATOMIC_ACQ_REL); }

void vnode_put(struct vnode *v) {
    if (!v) return;
    if (__atomic_sub_fetch(&v->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    if (v->ops->release) v->ops->release(v);
}

/* ---- names ---------------------------------------------------------------------- */

#include "../lib/nfc_table.h"

static u32 compose(u32 base, u32 mark) {
    u32 lo = 0, hi = ARRAY_SIZE(nfc_pairs);
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (nfc_pairs[mid].base == base && nfc_pairs[mid].mark == mark) return nfc_pairs[mid].composed;
        if (nfc_pairs[mid].base < base || (nfc_pairs[mid].base == base && nfc_pairs[mid].mark < mark)) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static u32 utf8_encode(u32 cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int vfs_normalize_name(const char *name, u32 len, char *out) {
    if (len == 0) return -ENOENT;
    u32 cps[NAME_MAX + 1], n = 0;
    for (u32 i = 0; i < len;) {
        u8 c = (u8)name[i];
        u32 cp, need;
        if (c < 0x80) cp = c, need = 0;
        else if ((c & 0xE0) == 0xC0) cp = c & 0x1F, need = 1;
        else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, need = 2;
        else if ((c & 0xF8) == 0xF0) cp = c & 0x07, need = 3;
        else return -EINVAL;
        if (i + need >= len + (need == 0)) return -EINVAL;
        for (u32 k = 1; k <= need; k++) {
            u8 cc = (u8)name[i + k];
            if ((cc & 0xC0) != 0x80) return -EINVAL;
            cp = cp << 6 | (cc & 0x3F);
        }
        static const u32 min_for[] = {0, 0x80, 0x800, 0x10000};
        if (cp < min_for[need] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -EINVAL;   /* overlong/invalid */
        if (cp == 0 || cp == '/') return -EINVAL;
        i += need + 1;
        u32 composed = n ? compose(cps[n - 1], cp) : 0;
        if (composed) cps[n - 1] = composed;
        else if (n < NAME_MAX) cps[n++] = cp;
        else return -ENAMETOOLONG;
    }
    u32 bytes = 0;
    for (u32 i = 0; i < n; i++) {
        char tmp[4];
        u32 k = utf8_encode(cps[i], tmp);
        if (bytes + k > NAME_MAX) return -ENAMETOOLONG;
        memcpy(out + bytes, tmp, k);
        bytes += k;
    }
    out[bytes] = 0;
    return (int)bytes;
}

static bool is_dot(const char *s, u32 len) { return (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'); }

/* ---- mounts and lookup --------------------------------------------------------- */

void vfs_init(void) { root = NULL; }

int vfs_mount_root(struct vnode *r) {
    root = r;
    vnode_ref(r);
    return 0;
}

struct vnode *vfs_root(void) { return root; }

int vfs_mount(const char *path, struct vnode *fs_root) {
    struct vnode *dir;
    int r = vfs_lookup(path, NULL, &dir);
    if (r < 0) return r;
    if (dir->type != VN_DIR) {
        vnode_put(dir);
        return -ENOTDIR;
    }
    dir->mounted = fs_root;   /* the mount point keeps its reference */
    fs_root->covers = dir;
    vnode_ref(fs_root);
    return 0;
}

static int may(struct vnode *v, int want, bool real) {
    if (!v->ops->stat) return 0;
    struct vnode_stat st;
    int r = v->ops->stat(v, &st);
    if (r < 0) return r;
    return cred_may(cred_current(), st.mode, st.uid, st.gid, want, real) ? 0 : -EACCES;
}

int vfs_permission(struct vnode *v, int want) { return may(v, want, false); }

int vfs_setattr(struct vnode *v, const struct vnode_attr *attr) {
    return v->ops->setattr ? v->ops->setattr(v, attr) : -EPERM;
}

/* Mode, owner and group of a file created in `parent`. */
static int new_attr(struct vnode *parent, int type, u32 mode, struct vnode_attr *a) {
    const struct cred *c = cred_current();
    struct vnode_stat st;
    int r = parent->ops->stat ? parent->ops->stat(parent, &st) : -EIO;
    if (r < 0) return r;
    a->mask = ATTR_MODE | ATTR_UID | ATTR_GID;
    a->mode = mode & 07777 & ~cred_umask();
    a->uid = c->euid;
    a->gid = c->egid;
    if (st.mode & S_ISGID) {   /* a set-group-id directory gives its group to new files */
        a->gid = st.gid;
        if (type == VN_DIR) a->mode |= S_ISGID;
    }
    return 0;
}

/* The sticky bit: in such a directory only root and the owners may delete or rename a file. */
static int sticky_ok(struct vnode *dir, const char *name, u32 len) {
    struct vnode_stat ds, ts;
    if (!dir->ops->stat || dir->ops->stat(dir, &ds) < 0 || !(ds.mode & S_ISVTX)) return 0;
    const struct cred *c = cred_current();
    if (c->euid == 0 || c->euid == ds.uid) return 0;
    struct vnode *t;
    if (dir->ops->lookup(dir, name, len, &t) < 0) return 0;   /* not there: the operation says so */
    int r = t->ops->stat && t->ops->stat(t, &ts) == 0 && ts.uid == c->euid ? 0 : -EPERM;
    vnode_put(t);
    return r;
}

/* One step of a path walk: `cur` (referenced) -> child (referenced). */
static int step(struct vnode *cur, const char *name, u32 len, struct vnode **next) {
    if (cur->type == VN_DIR && !(len == 1 && name[0] == '.')) {
        int r = may(cur, MAY_EXEC, false);   /* searching a directory needs its x bit */
        if (r < 0) return r;
    }
    if (len == 1 && name[0] == '.') {
        vnode_ref(cur);
        *next = cur;
        return 0;
    }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        if (cur == root) {
            vnode_ref(cur);
            *next = cur;
            return 0;
        }
        if (cur->covers) cur = cur->covers;   /* leave a mounted file system */
    }
    if (cur->type != VN_DIR) return -ENOTDIR;
    char norm[NAME_MAX + 1];
    int n = is_dot(name, len) ? (int)len : vfs_normalize_name(name, len, norm);
    if (n < 0) return n;
    if (is_dot(name, len)) memcpy(norm, name, len);
    struct vnode *child;
    int r = cur->ops->lookup(cur, norm, (u32)n, &child);
    if (r < 0) return r;
    if (child->mounted) {   /* enter the file system mounted here */
        struct vnode *m = child->mounted;
        vnode_ref(m);
        vnode_put(child);
        child = m;
    }
    *next = child;
    return 0;
}

/* Walks `path` up to (not including) its last component when `stop_before_last`. */
static int walk(const char *path, struct vnode *cwd, bool stop_before_last, struct vnode **out, const char **last,
                u32 *last_len) {
    if (!root) return -ENOENT;
    if (!path[0]) return -ENOENT;
    struct vnode *cur = (path[0] == '/' || !cwd) ? root : cwd;
    vnode_ref(cur);
    const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        u32 len = (u32)(p - start);
        const char *rest = p;
        while (*rest == '/') rest++;
        if (len == 0) break;                          /* trailing slash or the root itself */
        if (len > NAME_MAX) {
            vnode_put(cur);
            return -ENAMETOOLONG;
        }
        if (stop_before_last && !*rest) {
            *last = start;
            *last_len = len;
            *out = cur;
            return 0;
        }
        struct vnode *next;
        int r = step(cur, start, len, &next);
        vnode_put(cur);
        if (r < 0) return r;
        cur = next;
    }
    if (stop_before_last) {   /* "/" or "dir/": there is no final name */
        vnode_put(cur);
        return -EINVAL;
    }
    *out = cur;
    return 0;
}

int vfs_lookup(const char *path, struct vnode *cwd, struct vnode **out) {
    return walk(path, cwd, false, out, NULL, NULL);
}

int vfs_lookup_parent(const char *path, struct vnode *cwd, struct vnode **parent, char *name, u32 *len) {
    const char *last;
    u32 last_len;
    int r = walk(path, cwd, true, parent, &last, &last_len);
    if (r < 0) return r;
    if (is_dot(last, last_len)) {
        vnode_put(*parent);
        return -EINVAL;
    }
    int n = vfs_normalize_name(last, last_len, name);
    if (n < 0) {
        vnode_put(*parent);
        return n;
    }
    if ((*parent)->type != VN_DIR) {
        vnode_put(*parent);
        return -ENOTDIR;
    }
    *len = (u32)n;
    return 0;
}

/* ---- open files ------------------------------------------------------------------ */

struct file *file_new(struct vnode *v, u32 flags) {
    struct file *f = kcalloc(1, sizeof *f);
    if (!f) {
        vnode_put(v);
        return NULL;
    }
    f->vn = v;
    f->flags = flags;
    f->refs = 1;
    return f;
}

void file_ref(struct file *f) { __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL); }

void file_put(struct file *f) {
    if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    if (f->vn->ops->close) f->vn->ops->close(f->vn, f->flags);
    vnode_put(f->vn);
    kfree(f);
}

int vfs_open(const char *path, struct vnode *cwd, u32 flags, u32 mode, struct file **out) {
    struct vnode *v;
    int r = vfs_lookup(path, cwd, &v);
    bool created = false;
    if (r == -ENOENT && (flags & O_CREAT)) {
        struct vnode *parent;
        char name[NAME_MAX + 1];
        u32 len;
        r = vfs_lookup_parent(path, cwd, &parent, name, &len);
        if (r < 0) return r;
        struct vnode_attr attr;
        r = may(parent, MAY_WRITE | MAY_EXEC, false);
        if (r == 0) r = new_attr(parent, VN_FILE, mode, &attr);
        if (r == 0) r = parent->ops->create ? parent->ops->create(parent, name, len, VN_FILE, &attr, &v) : -EPERM;
        created = r == 0;
        vnode_put(parent);
        if (r == -EEXIST && !(flags & O_EXCL)) r = vfs_lookup(path, cwd, &v);   /* created meanwhile */
        if (r < 0) return r;
    } else if (r < 0) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_put(v);
        return -EEXIST;
    }
    u32 acc = flags & O_ACCMODE;
    if (v->type == VN_DIR && acc != O_RDONLY) {
        vnode_put(v);
        return -EISDIR;
    }
    if (!created) {   /* the creator may write its new file even if the mode says otherwise */
        int want = (acc != O_WRONLY ? MAY_READ : 0) | (acc != O_RDONLY || (flags & O_TRUNC) ? MAY_WRITE : 0);
        r = v->type == VN_PIPE ? 0 : may(v, want, false);
        if (r < 0) {
            vnode_put(v);
            return r;
        }
    }
    if ((flags & O_DIRECTORY) && v->type != VN_DIR) {
        vnode_put(v);
        return -ENOTDIR;
    }
    if ((flags & O_TRUNC) && acc != O_RDONLY && v->type == VN_FILE && v->ops->truncate) {
        r = v->ops->truncate(v, 0);
        if (r < 0) {
            vnode_put(v);
            return r;
        }
    }
    *out = file_new(v, flags);
    return *out ? 0 : -ENOMEM;
}

/* ---- poll ------------------------------------------------------------------------------ */

static volatile u64 poll_generation;
static struct waitqueue poll_wq;

void vfs_poll_notify(void) {
    __atomic_add_fetch(&poll_generation, 1, __ATOMIC_ACQ_REL);
    waitqueue_wake_all(&poll_wq);
}

u64 vfs_poll_generation(void) { return __atomic_load_n(&poll_generation, __ATOMIC_ACQUIRE); }

struct waitqueue *vfs_poll_queue(void) { return &poll_wq; }

u32 file_poll(struct file *f) {
    u32 acc = f->flags & O_ACCMODE;
    u32 all = (acc != O_WRONLY ? POLLIN : 0) | (acc != O_RDONLY ? POLLOUT : 0);
    return f->vn->ops->poll ? f->vn->ops->poll(f->vn, f->flags) : all;
}

/* A file with shared mappings: its mapped pages hold the newest data (see opaline/uvm.h). */
i64 vnode_read_at(struct vnode *v, u64 offset, void *buf, u64 len) {
    if (!v->ops->read) return -EINVAL;
    if (v->memobj) memobj_sync(v->memobj);   /* what the mappings wrote goes to the file first */
    return v->ops->read(v, offset, buf, len);
}

i64 vnode_write_at(struct vnode *v, u64 offset, const void *buf, u64 len) {
    if (!v->ops->write) return -EINVAL;
    i64 r = v->ops->write(v, offset, buf, len);
    if (r > 0 && v->memobj) memobj_written(v->memobj, offset, buf, (u64)r);   /* the mappings see it */
    return r;
}

i64 file_read(struct file *f, void *buf, u64 len) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (!f->vn->ops->read) return -EINVAL;
    i64 r = vnode_read_at(f->vn, f->offset, buf, len);
    if (r > 0 && f->vn->type == VN_FILE) f->offset += (u64)r;
    return r;
}

i64 file_write(struct file *f, const void *buf, u64 len) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!f->vn->ops->write) return -EINVAL;
    if ((f->flags & O_APPEND) && f->vn->type == VN_FILE) {
        struct vnode_stat st;
        if (f->vn->ops->stat(f->vn, &st) == 0) f->offset = st.size;
    }
    i64 r = vnode_write_at(f->vn, f->offset, buf, len);
    if (r > 0 && f->vn->type == VN_FILE) f->offset += (u64)r;
    return r;
}

/* ---- path operations ---------------------------------------------------------------- */

int vfs_mkdir(const char *path, struct vnode *cwd, u32 mode) {
    struct vnode *parent, *v;
    char name[NAME_MAX + 1];
    u32 len;
    int r = vfs_lookup_parent(path, cwd, &parent, name, &len);
    if (r < 0) return r;
    struct vnode_attr attr;
    r = may(parent, MAY_WRITE | MAY_EXEC, false);
    if (r == 0) r = new_attr(parent, VN_DIR, mode, &attr);
    if (r == 0) r = parent->ops->create ? parent->ops->create(parent, name, len, VN_DIR, &attr, &v) : -EPERM;
    vnode_put(parent);
    if (r == 0) vnode_put(v);
    return r;
}

int vfs_remove(const char *path, struct vnode *cwd, bool is_dir) {
    struct vnode *parent;
    char name[NAME_MAX + 1];
    u32 len;
    int r = vfs_lookup_parent(path, cwd, &parent, name, &len);
    if (r < 0) return r;
    r = may(parent, MAY_WRITE | MAY_EXEC, false);
    if (r == 0) r = sticky_ok(parent, name, len);
    if (r < 0) {
        vnode_put(parent);
        return r;
    }
    struct vnode *target;
    if (parent->ops->lookup(parent, name, len, &target) == 0) {
        bool busy = target->mounted != NULL || target == root;
        vnode_put(target);
        if (busy) {
            vnode_put(parent);
            return -EBUSY;
        }
    }
    r = parent->ops->remove ? parent->ops->remove(parent, name, len, is_dir) : -EPERM;
    vnode_put(parent);
    return r;
}

int vfs_rename(const char *old_path, const char *new_path, struct vnode *cwd) {
    struct vnode *od, *nd;
    char on[NAME_MAX + 1], nn[NAME_MAX + 1];
    u32 ol, nl;
    int r = vfs_lookup_parent(old_path, cwd, &od, on, &ol);
    if (r < 0) return r;
    r = vfs_lookup_parent(new_path, cwd, &nd, nn, &nl);
    if (r < 0) {
        vnode_put(od);
        return r;
    }
    if (od->fs != nd->fs) r = -EXDEV;
    else if ((r = may(od, MAY_WRITE | MAY_EXEC, false)) < 0 || (r = may(nd, MAY_WRITE | MAY_EXEC, false)) < 0) {}
    else if ((r = sticky_ok(od, on, ol)) < 0 || (r = sticky_ok(nd, nn, nl)) < 0) {}
    else r = od->ops->rename ? od->ops->rename(od, on, ol, nd, nn, nl) : -EPERM;
    vnode_put(od);
    vnode_put(nd);
    return r;
}

int vfs_stat(const char *path, struct vnode *cwd, struct vnode_stat *out) {
    struct vnode *v;
    int r = vfs_lookup(path, cwd, &v);
    if (r < 0) return r;
    r = v->ops->stat(v, out);
    vnode_put(v);
    return r;
}

int vfs_access(const char *path, struct vnode *cwd, int want, bool real_ids) {
    struct vnode *v;
    int r = vfs_lookup(path, cwd, &v);
    if (r < 0) return r;
    r = want ? may(v, want, real_ids) : 0;
    vnode_put(v);
    return r;
}

int vfs_read_file(const char *path, struct vnode *cwd, void **data, u64 *size) {
    struct vnode *v;
    int r = vfs_lookup(path, cwd, &v);
    if (r < 0) return r;
    struct vnode_stat st;
    if (v->type != VN_FILE) r = v->type == VN_DIR ? -EISDIR : -EINVAL;
    else r = v->ops->stat(v, &st);
    if (r < 0) {
        vnode_put(v);
        return r;
    }
    void *buf = kmalloc(st.size ? st.size : 1);
    if (!buf) {
        vnode_put(v);
        return -ENOMEM;
    }
    i64 n = v->ops->read(v, 0, buf, st.size);
    vnode_put(v);
    if (n < 0 || (u64)n != st.size) {
        kfree(buf);
        return n < 0 ? (int)n : -EIO;
    }
    *data = buf;
    *size = st.size;
    return 0;
}
