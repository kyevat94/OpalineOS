/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Virtual file system.
 *
 * Every file, directory, device and pipe is a vnode with a table of
 * operations. File systems: EFS3 on the root partition (kernel/fs/efs3.c) and
 * devfs at /dev (console, tty, null, zero). Each file system does its own
 * locking inside its operations, because some operations block (a pipe or
 * terminal read waits for data).
 *
 * Names are UTF-8. Names given to create/lookup are validated and composed to
 * NFC first (vfs_normalize_name), so "ação" written with combining accents is
 * the same file.
 *
 * Errors are negative errno values (opaline/errno.h).
 */
#ifndef OPALINE_VFS_H
#define OPALINE_VFS_H

#include <opaline/types.h>

#define NAME_MAX  255
#define PATH_MAX  1024
#define MAX_FDS   64

enum vnode_type { VN_FILE = 1, VN_DIR, VN_CHARDEV, VN_PIPE };

/* Linux open() flags */
#define O_ACCMODE   0x0003
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

struct vnode;

struct dir_entry {
    u64 ino;
    u8 type;                    /* enum vnode_type */
    u16 name_len;
    char name[NAME_MAX + 1];
};

struct vnode_stat {
    u64 ino;
    u32 mode;                   /* type bits + permissions, as in Linux st_mode */
    u32 links;
    u64 size;
    u64 blocks;                 /* 512-byte units */
    u64 dev;
    u64 rdev;
};

struct vnode_ops {
    i64 (*read)(struct vnode *v, u64 offset, void *buf, u64 len);
    i64 (*write)(struct vnode *v, u64 offset, const void *buf, u64 len);
    int (*truncate)(struct vnode *v, u64 size);
    int (*lookup)(struct vnode *dir, const char *name, u32 len, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, u32 len, int type, struct vnode **out);
    int (*remove)(struct vnode *dir, const char *name, u32 len, bool is_dir);
    int (*rename)(struct vnode *old_dir, const char *old_name, u32 old_len, struct vnode *new_dir,
                  const char *new_name, u32 new_len);
    int (*readdir)(struct vnode *dir, u64 *cookie, struct dir_entry *out);   /* 1 entry, 0 end, <0 error */
    int (*stat)(struct vnode *v, struct vnode_stat *out);
    int (*ioctl)(struct vnode *v, u64 request, u64 arg);
    int (*sync)(struct vnode *v);
    void (*close)(struct vnode *v, u32 open_flags);   /* an open file of this vnode was closed */
    void (*release)(struct vnode *v);                  /* the last reference is gone */
};

struct vnode {
    int type;
    int refs;
    u64 ino;
    const struct vnode_ops *ops;
    void *fs;                   /* file system instance */
    void *priv;
    struct vnode *mounted;      /* a file system mounted on this directory */
    struct vnode *covers;       /* for a mounted root: the directory it is mounted on */
    struct vnode *cache_next;   /* used by the file system's vnode cache */
};

/* An open file (shared by dup'ed descriptors). */
struct file {
    struct vnode *vn;
    u64 offset;
    u32 flags;
    int refs;
};

void vnode_ref(struct vnode *v);
void vnode_put(struct vnode *v);

/* Validates a name component and composes it to NFC into `out` (NAME_MAX + 1 bytes). Returns its length or -errno. */
int vfs_normalize_name(const char *name, u32 len, char *out);

void vfs_init(void);
int vfs_mount_root(struct vnode *root);
int vfs_mount(const char *path, struct vnode *root);
struct vnode *vfs_root(void);

/* Path lookup relative to `cwd` (NULL = root). */
int vfs_lookup(const char *path, struct vnode *cwd, struct vnode **out);
/* Finds the parent directory of `path` and copies the final (normalized) name. */
int vfs_lookup_parent(const char *path, struct vnode *cwd, struct vnode **parent, char *name, u32 *len);

int vfs_open(const char *path, struct vnode *cwd, u32 flags, struct file **out);
struct file *file_new(struct vnode *v, u32 flags);   /* takes over the reference to v */
void file_ref(struct file *f);
void file_put(struct file *f);
i64 file_read(struct file *f, void *buf, u64 len);
i64 file_write(struct file *f, const void *buf, u64 len);

int vfs_mkdir(const char *path, struct vnode *cwd);
int vfs_remove(const char *path, struct vnode *cwd, bool is_dir);
int vfs_rename(const char *old_path, const char *new_path, struct vnode *cwd);
int vfs_stat(const char *path, struct vnode *cwd, struct vnode_stat *out);

/* Reads a whole file into a kmalloc'd buffer (for the program loader). */
int vfs_read_file(const char *path, struct vnode *cwd, void **data, u64 *size);

/* File systems and devices */
struct block_device;
int efs3_mount(struct block_device *dev, struct vnode **root);
void efs3_info(struct vnode *root, u64 *total_blocks, u64 *free_blocks, char *label, u32 label_size);
struct vnode *devfs_init(void);
int pipe_create(struct file **read_end, struct file **write_end);
struct vnode *tty_vnode(void);
void tty_input(const char *utf8, u32 len);     /* keyboard -> terminal */
void tty_init(void);
void keyboard_init(void);

#endif
