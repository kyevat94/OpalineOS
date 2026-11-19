/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* /dev: console and tty (the terminal), null and zero. */
#include <opaline/errno.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

static i64 null_read(struct vnode *v, u64 off, void *buf, u64 len) {
    (void)v, (void)off, (void)buf, (void)len;
    return 0;
}

static i64 null_write(struct vnode *v, u64 off, const void *buf, u64 len) {
    (void)v, (void)off, (void)buf;
    return (i64)len;
}

static i64 zero_read(struct vnode *v, u64 off, void *buf, u64 len) {
    (void)v, (void)off;
    memset(buf, 0, len);
    return (i64)len;
}

static int dev_stat(struct vnode *v, struct vnode_stat *st) {
    memset(st, 0, sizeof *st);
    st->ino = v->ino;
    st->mode = v->type == VN_DIR ? 0x4000 | 0755 : 0x2000 | 0666;   /* S_IFDIR / S_IFCHR */
    st->links = 1;
    st->dev = 2;
    st->rdev = v->ino;
    return 0;
}

static const struct vnode_ops null_ops = {.read = null_read, .write = null_write, .stat = dev_stat};
static const struct vnode_ops zero_ops = {.read = zero_read, .write = null_write, .stat = dev_stat};

static struct vnode null_vnode = {.type = VN_CHARDEV, .refs = 1, .ino = 3, .ops = &null_ops};
static struct vnode zero_vnode = {.type = VN_CHARDEV, .refs = 1, .ino = 4, .ops = &zero_ops};

static struct {
    const char *name;
    struct vnode *(*get)(void);
} entries[4];

static struct vnode *get_null(void) { return &null_vnode; }
static struct vnode *get_zero(void) { return &zero_vnode; }

static int devfs_lookup(struct vnode *dir, const char *name, u32 len, struct vnode **out) {
    for (unsigned i = 0; i < ARRAY_SIZE(entries); i++)
        if (strlen(entries[i].name) == len && !memcmp(entries[i].name, name, len)) {
            *out = entries[i].get();
            vnode_ref(*out);
            return 0;
        }
    if (len == 1 && name[0] == '.') {
        vnode_ref(dir);
        *out = dir;
        return 0;
    }
    return -ENOENT;
}

static int devfs_readdir(struct vnode *dir, u64 *cookie, struct dir_entry *out) {
    (void)dir;
    if (*cookie >= ARRAY_SIZE(entries)) return 0;
    struct vnode *v = entries[*cookie].get();
    out->ino = v->ino;
    out->type = (u8)v->type;
    out->name_len = (u16)strlen(entries[*cookie].name);
    memcpy(out->name, entries[*cookie].name, out->name_len + 1);
    (*cookie)++;
    return 1;
}

static const struct vnode_ops dir_ops = {.lookup = devfs_lookup, .readdir = devfs_readdir, .stat = dev_stat};
static struct vnode devfs_root = {.type = VN_DIR, .refs = 1, .ino = 1, .ops = &dir_ops};

struct vnode *devfs_init(void) {
    entries[0].name = "console";
    entries[0].get = tty_vnode;
    entries[1].name = "tty";
    entries[1].get = tty_vnode;
    entries[2].name = "null";
    entries[2].get = get_null;
    entries[3].name = "zero";
    entries[3].get = get_zero;
    return &devfs_root;
}
