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
    if (v->ops->put) {
        v->ops->put(v);
        return;
    }
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

#define SYMLINK_MAX 40

/*
 * Walks `path` from `cwd`, following symbolic links (a link's target replaces it in the
 * path: relative targets start from the link's directory). The last component is followed
 * only with W_FOLLOW, or if a slash comes after it (then it must also be a directory, as in
 * Linux). With W_PARENT, stops before the last component, returns its directory and copies
 * the name (raw) into `last` (NAME_MAX + 1 bytes), and into `last_slash` (if not NULL) whether a
 * slash came after it: the caller decides what that means. W_CREATE (with W_PARENT, for open with
 * O_CREAT) returns 0 and the file if the last component exists (a link to it followed with
 * W_FOLLOW, so a dangling link names the file to create), else 1 and its directory and name.
 */
#define W_PARENT 1
#define W_FOLLOW 2
#define W_CREATE 4

static int walk(const char *path, struct vnode *cwd, int flags, struct vnode **out, char *last, u32 *last_len,
                bool *last_slash) {
    bool want_parent = flags & W_PARENT, create = flags & W_CREATE;
    if (!root) return -ENOENT;
    if (!path[0]) return -ENOENT;
    char *buf = NULL;   /* the path once a link was expanded */
    int links = 0, r = 0;
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
            r = -ENAMETOOLONG;
            goto fail;
        }
        bool final = !*rest, dir_slash = final && *p == '/';   /* "name/": must be a directory */
        if (create && dir_slash) {   /* O_CREAT cannot make a directory */
            r = -EISDIR;
            goto fail;
        }
        struct vnode *next;
        if (want_parent && final) {
            r = create ? step(cur, start, len, &next) : -ENOENT;
            if (r == -ENOENT) {
                memcpy(last, start, len);
                last[len] = 0;
                *last_len = len;
                if (last_slash) *last_slash = dir_slash;
                *out = cur;
                kfree(buf);
                return create ? 1 : 0;
            }
        } else {
            r = step(cur, start, len, &next);
        }
        if (r < 0) goto fail;
        if (next->type == VN_SYMLINK && (!final || dir_slash || (flags & W_FOLLOW))) {
            char *target = kmalloc(PATH_MAX), *nb = kmalloc(PATH_MAX);
            int n = !target || !nb ? -ENOMEM : ++links > SYMLINK_MAX ? -ELOOP
                  : next->ops->readlink ? next->ops->readlink(next, target, PATH_MAX - 1) : -EINVAL;
            vnode_put(next);
            u64 rlen = strlen(p);   /* from the slash on: a trailing slash stays */
            if (n == 0) n = -ENOENT;
            if (n > 0 && (u64)n + rlen >= PATH_MAX) n = -ENAMETOOLONG;
            if (n < 0) {
                kfree(target);
                kfree(nb);
                r = n;
                goto fail;
            }
            memcpy(nb, target, (u64)n);   /* the target, then what was left of the path */
            memcpy(nb + n, p, rlen);
            nb[(u64)n + rlen] = 0;
            kfree(target);
            kfree(buf);
            buf = nb;
            p = nb;
            if (nb[0] == '/') {   /* absolute target: from the root again */
                vnode_put(cur);
                cur = root;
                vnode_ref(cur);
            }
            continue;   /* a relative target starts from the link's directory, which is cur */
        }
        if (dir_slash && next->type != VN_DIR) {
            vnode_put(next);
            r = -ENOTDIR;
            goto fail;
        }
        vnode_put(cur);
        cur = next;
    }
    if (want_parent && !create) {   /* "/": there is no final name */
        r = -EINVAL;
        goto fail;
    }
    *out = cur;
    kfree(buf);
    return 0;
fail:
    vnode_put(cur);
    kfree(buf);
    return r;
}

int vfs_lookup(const char *path, struct vnode *cwd, struct vnode **out) {
    return walk(path, cwd, W_FOLLOW, out, NULL, NULL, NULL);
}

int vfs_lookup_nofollow(const char *path, struct vnode *cwd, struct vnode **out) {
    return walk(path, cwd, 0, out, NULL, NULL, NULL);
}

/* vfs_lookup_parent, and *slash: whether a slash followed the name ("name/"). */
static int lookup_parent(const char *path, struct vnode *cwd, struct vnode **parent, char *name, u32 *len, bool *slash) {
    char raw[NAME_MAX + 1];
    u32 raw_len;
    *slash = false;
    int r = walk(path, cwd, W_PARENT | W_FOLLOW, parent, raw, &raw_len, slash);
    if (r < 0) return r;
    if (is_dot(raw, raw_len)) {
        vnode_put(*parent);
        return -EINVAL;
    }
    int n = vfs_normalize_name(raw, raw_len, name);
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

int vfs_lookup_parent(const char *path, struct vnode *cwd, struct vnode **parent, char *name, u32 *len) {
    bool slash;
    return lookup_parent(path, cwd, parent, name, len, &slash);
}

/* For a name written with a trailing slash ("name/" asks for a directory): 0 if it is one, else
   -ENOENT or -ENOTDIR. As in Linux, a link there is not followed: "link/" is not a directory. */
static int must_be_dir(struct vnode *dir, const char *name, u32 len) {
    struct vnode *t;
    if (dir->ops->lookup(dir, name, len, &t) < 0) return -ENOENT;
    int r = t->type == VN_DIR ? 0 : -ENOTDIR;
    vnode_put(t);
    return r;
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

/* open() with O_CREAT: the existing file, or a new one. As in Linux, a dangling link names the
   file to create, and with O_EXCL (or O_NOFOLLOW) a link is not followed: it exists. */
static int open_create(const char *path, struct vnode *cwd, u32 flags, u32 mode, struct vnode **out, bool *created) {
    int walk_flags = W_PARENT | W_CREATE | (flags & (O_EXCL | O_NOFOLLOW) ? 0 : W_FOLLOW);
    for (int tries = 0;; tries++) {
        struct vnode *parent;
        char raw[NAME_MAX + 1], name[NAME_MAX + 1];
        u32 raw_len;
        int r = walk(path, cwd, walk_flags, &parent, raw, &raw_len, NULL);
        if (r < 0) return r;
        if (r == 0) {   /* it exists */
            if (flags & O_EXCL) {
                vnode_put(parent);
                return -EEXIST;
            }
            *out = parent;
            return 0;
        }
        int len = vfs_normalize_name(raw, raw_len, name);
        struct vnode_attr attr;
        r = len < 0 ? len : may(parent, MAY_WRITE | MAY_EXEC, false);
        if (r == 0) r = new_attr(parent, VN_FILE, mode, &attr);
        if (r == 0) r = parent->ops->create ? parent->ops->create(parent, name, (u32)len, VN_FILE, &attr, out) : -EPERM;
        vnode_put(parent);
        if (r == -EEXIST && !(flags & O_EXCL) && tries < 3) continue;   /* created meanwhile */
        *created = r == 0;
        return r;
    }
}

int vfs_open(const char *path, struct vnode *cwd, u32 flags, u32 mode, struct file **out) {
    struct vnode *v;
    bool created = false;
    int r = (flags & O_CREAT) ? open_create(path, cwd, flags, mode, &v, &created)
          : (flags & O_NOFOLLOW) ? vfs_lookup_nofollow(path, cwd, &v) : vfs_lookup(path, cwd, &v);
    if (r < 0) return r;
    if (v->type == VN_SYMLINK) {   /* only with O_NOFOLLOW: a link cannot be opened */
        vnode_put(v);
        return -ELOOP;
    }
    u32 acc = flags & O_ACCMODE;
    if (v->type == VN_DIR && (acc != O_RDONLY || (flags & O_CREAT))) {   /* O_CREAT on a directory too (Linux) */
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
    struct memobj *obj = vnode_memobj_get(v);   /* referenced: the last munmap may free it meanwhile */
    i64 r = v->ops->read(v, offset, buf, len);
    if (obj) {
        /* what the mappings wrote replaces the file data (syncing every page here would make each read
           cost the whole cache) */
        if (r > 0) memobj_read(obj, offset, buf, (u64)r);
        memobj_put(obj);
    }
    return r;
}

i64 vnode_write_at(struct vnode *v, u64 offset, const void *buf, u64 len) {
    if (!v->ops->write) return -EINVAL;
    i64 r = v->ops->write(v, offset, buf, len);
    struct memobj *obj = r > 0 ? vnode_memobj_get(v) : NULL;
    if (obj) {
        memobj_written(obj, offset, buf, (u64)r);   /* the mappings see it */
        memobj_put(obj);
    }
    return r;
}

i64 file_read(struct file *f, void *buf, u64 len) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (!f->vn->ops->read) return -EINVAL;
    struct thread *t = thread_current();
    bool nb = t->io_nonblock;
    t->io_nonblock = f->flags & O_NONBLOCK;   /* pipes and the terminal look at it instead of waiting */
    i64 r = vnode_read_at(f->vn, f->offset, buf, len);
    t->io_nonblock = nb;
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
    struct thread *t = thread_current();
    bool nb = t->io_nonblock;
    t->io_nonblock = f->flags & O_NONBLOCK;
    i64 r = vnode_write_at(f->vn, f->offset, buf, len);
    t->io_nonblock = nb;
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

/* The last name in `path` is "." or ".." (trailing slashes aside). */
static bool ends_in_dot(const char *path) {
    u64 n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    u64 at = n;
    while (at > 0 && path[at - 1] != '/') at--;
    return is_dot(path + at, (u32)(n - at));
}

int vfs_remove(const char *path, struct vnode *cwd, bool is_dir) {
    struct vnode *parent;
    char name[NAME_MAX + 1];
    u32 len;
    bool slash;
    int r = lookup_parent(path, cwd, &parent, name, &len, &slash);
    if (r == -EINVAL && !is_dir && ends_in_dot(path)) return -EISDIR;   /* unlink(".") (Linux); rmdir(".") stays EINVAL */
    if (r < 0) return r;
    if (slash && !is_dir) {   /* unlink("name/") removes nothing (Linux): ENOENT, EISDIR or ENOTDIR */
        r = must_be_dir(parent, name, len);
        vnode_put(parent);
        return r == 0 ? -EISDIR : r;
    }
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

/* A mount point or the root cannot be replaced. */
static int not_busy(struct vnode *dir, const char *name, u32 len) {
    struct vnode *t;
    if (dir->ops->lookup(dir, name, len, &t) < 0) return 0;
    bool busy = t->mounted != NULL || t == root;
    vnode_put(t);
    return busy ? -EBUSY : 0;
}

int vfs_rename(const char *old_path, const char *new_path, struct vnode *cwd) {
    struct vnode *od, *nd;
    char on[NAME_MAX + 1], nn[NAME_MAX + 1];
    u32 ol, nl;
    bool os, ns;
    int r = lookup_parent(old_path, cwd, &od, on, &ol, &os);
    if (r < 0) return r;
    r = lookup_parent(new_path, cwd, &nd, nn, &nl, &ns);
    if (r < 0) {
        vnode_put(od);
        return r;
    }
    if (od->fs != nd->fs) r = -EXDEV;
    else if ((os || ns) && (r = must_be_dir(od, on, ol)) < 0) {}   /* a slash on either name: only a directory moves */
    else if ((r = may(od, MAY_WRITE | MAY_EXEC, false)) < 0 || (r = may(nd, MAY_WRITE | MAY_EXEC, false)) < 0) {}
    else if ((r = sticky_ok(od, on, ol)) < 0 || (r = sticky_ok(nd, nn, nl)) < 0) {}
    else if ((r = not_busy(nd, nn, nl)) < 0) {}   /* a directory may now replace an empty one */
    else r =od->ops->rename ? od->ops->rename(od, on, ol, nd, nn, nl) : -EPERM;
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

int vfs_lstat(const char *path, struct vnode *cwd, struct vnode_stat *out) {
    struct vnode *v;
    int r = vfs_lookup_nofollow(path, cwd, &v);
    if (r < 0) return r;
    r = v->ops->stat(v, out);
    vnode_put(v);
    return r;
}

int vfs_symlink(const char *target, const char *path, struct vnode *cwd) {
    if (!target[0]) return -ENOENT;
    struct vnode *parent;
    char name[NAME_MAX + 1];
    u32 len;
    bool slash;
    int r = lookup_parent(path, cwd, &parent, name, &len, &slash);
    if (r < 0) return r;
    if (slash) {   /* "name/" asks for a directory, which symlink cannot make (Linux): EEXIST or ENOENT */
        r = must_be_dir(parent, name, len) == -ENOENT ? -ENOENT : -EEXIST;
        vnode_put(parent);
        return r;
    }
    struct vnode_attr attr;
    r = may(parent, MAY_WRITE | MAY_EXEC, false);
    if (r == 0) r = new_attr(parent, VN_SYMLINK, 0777, &attr);
    attr.mode = 0777;   /* links are always rwx for everyone: the target's mode is what counts */
    if (r == 0) r = parent->ops->symlink ? parent->ops->symlink(parent, name, len, target, &attr) : -EPERM;
    vnode_put(parent);
    return r;
}

int vfs_readlink(const char *path, struct vnode *cwd, char *buf, u32 size) {
    struct vnode *v;
    int r = vfs_lookup_nofollow(path, cwd, &v);
    if (r < 0) return r;
    r = v->type == VN_SYMLINK && v->ops->readlink ? v->ops->readlink(v, buf, size) : -EINVAL;
    vnode_put(v);
    return r;
}

int vfs_link(const char *old_path, struct vnode *old_cwd, const char *new_path, struct vnode *new_cwd, bool follow) {
    struct vnode *v, *parent;
    int r = follow ? vfs_lookup(old_path, old_cwd, &v) : vfs_lookup_nofollow(old_path, old_cwd, &v);
    if (r < 0) return r;
    if (v->type == VN_DIR) {
        vnode_put(v);
        return -EPERM;   /* no hard links to directories */
    }
    char name[NAME_MAX + 1];
    u32 len;
    bool slash;
    r = lookup_parent(new_path, new_cwd, &parent, name, &len, &slash);
    if (r < 0) {
        vnode_put(v);
        return r;
    }
    if (slash) r = must_be_dir(parent, name, len) == -ENOENT ? -ENOENT : -EEXIST;   /* as symlink */
    else r = may(parent, MAY_WRITE | MAY_EXEC, false);
    if (r == 0 && parent->fs != v->fs) r = -EXDEV;
    if (r == 0) r = parent->ops->link ? parent->ops->link(parent, name, len, v) : -EPERM;
    vnode_put(parent);
    vnode_put(v);
    return r;
}

int vfs_path_of(struct vnode *dir, char *out, u64 size) {
    char *tmp = kmalloc(PATH_MAX);
    struct dir_entry *de = kmalloc(sizeof *de);
    if (!tmp || !de) {
        kfree(tmp);
        kfree(de);
        return -ENOMEM;
    }
    u64 pos = PATH_MAX - 1;
    tmp[pos] = 0;
    struct vnode *cur = dir;
    vnode_ref(cur);
    int r = 0;
    for (int depth = 0; cur != root; depth++) {
        if (depth > 256) {
            r = -ELOOP;
            break;
        }
        if (cur->covers) {   /* the root of a mounted file system: continue from the mount point */
            struct vnode *c = cur->covers;
            vnode_ref(c);
            vnode_put(cur);
            cur = c;
            continue;
        }
        struct vnode *parent;
        r = cur->ops->lookup ? cur->ops->lookup(cur, "..", 2, &parent) : -ENOENT;
        if (r < 0) break;
        u64 cookie = 0;
        bool found = false;
        while ((r = parent->ops->readdir(parent, &cookie, de)) > 0)
            if (de->ino == cur->ino && !is_dot(de->name, de->name_len)) {
                found = true;
                break;
            }
        if (!found || pos < (u64)de->name_len + 2) {
            vnode_put(parent);
            r = r < 0 ? r : !found ? -ENOENT : -ENAMETOOLONG;
            break;
        }
        pos -= de->name_len;
        memcpy(tmp + pos, de->name, de->name_len);
        tmp[--pos] = '/';
        vnode_put(cur);
        cur = parent;
        r = 0;
    }
    vnode_put(cur);
    if (r == 0) {
        if (pos == PATH_MAX - 1) tmp[--pos] = '/';   /* the root itself */
        u64 n = PATH_MAX - pos;                      /* with the terminating zero */
        if (n > size) r = -ERANGE;
        else memcpy(out, tmp + pos, n);
    }
    kfree(tmp);
    kfree(de);
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
    r = vfs_read_vnode(v, data, size);
    vnode_put(v);
    return r;
}

int vfs_read_vnode(struct vnode *v, void **data, u64 *size) {
    struct vnode_stat st;
    int r;
    if (v->type != VN_FILE) r = v->type == VN_DIR ? -EISDIR : -EINVAL;
    else r = v->ops->stat(v, &st);
    if (r < 0) return r;
    void *buf = kmalloc(st.size ? st.size : 1);
    if (!buf) return -ENOMEM;
    i64 n = vnode_read_at(v, 0, buf, st.size);   /* with what shared mappings wrote, as read() sees it */
    if (n < 0 || (u64)n != st.size) {
        kfree(buf);
        return n < 0 ? (int)n : -EIO;
    }
    *data = buf;
    *size = st.size;
    return 0;
}
