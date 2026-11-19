/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * EFS3 file system driver. The on-disk format is documented in tools/efs3.py
 * (the Python implementation that creates the images).
 *
 * Every operation runs under one mutex per mounted file system. Inodes are not
 * kept in memory: they are read and written through the block cache, which is
 * write-through, so each operation is on disk when it returns. There is no
 * journal yet: a power cut in the middle of an operation can leave
 * inconsistencies.
 *
 * A file that is deleted while open keeps its data until the last reference
 * to it is dropped (as in POSIX).
 */
#include <opaline/block.h>
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

#define INODE_SIZE 128
#define PTRS (BLOCK_SIZE / 4)
#define S_IFMT  0xF000
#define S_IFDIR 0x4000
#define S_IFREG 0x8000
#define FT_FILE 1
#define FT_DIR  2

struct efs3_super {
    char magic[4];
    u32 version, block_size, block_count, inode_count, inode_bitmap, block_bitmap, block_bitmap_blocks;
    u32 inode_table, inode_table_blocks, first_data, root_inode, free_blocks, free_inodes, reserved;
    char label[32];
    u8 uuid[16];
} PACKED;

struct efs3_inode {
    u16 mode, links;
    u32 flags;
    u64 size, ctime, mtime;
    u32 direct[12];
    u32 indirect, dindirect;
    u8 reserved[40];
} PACKED;
_Static_assert(sizeof(struct efs3_inode) == INODE_SIZE, "EFS3 inode size");

struct dirent_raw {
    u32 ino;
    u16 rec_len;
    u8 name_len;
    u8 type;
    char name[];
} PACKED;

struct efs3 {
    struct block_device *dev;
    struct efs3_super sb;
    struct mutex lock;
    struct vnode *cache;          /* vnodes of this file system, one per inode */
    u32 block_hint, inode_hint;
};

static const struct vnode_ops efs3_ops;
static u8 zero_block[BLOCK_SIZE] ALIGNED(16);

/* ---- superblock, inodes, bitmaps ------------------------------------------ */

static bool write_super(struct efs3 *fs) {
    u8 *b = block_read(fs->dev, 0);
    if (!b) return false;
    memcpy(b, &fs->sb, sizeof fs->sb);
    return block_write(fs->dev, 0, b);
}

static bool inode_get(struct efs3 *fs, u32 ino, struct efs3_inode *out) {
    if (ino == 0 || ino >= fs->sb.inode_count) return false;
    u64 pos = (u64)ino * INODE_SIZE;
    u8 *b = block_read(fs->dev, fs->sb.inode_table + pos / BLOCK_SIZE);
    if (!b) return false;
    memcpy(out, b + pos % BLOCK_SIZE, INODE_SIZE);
    return true;
}

static bool inode_put(struct efs3 *fs, u32 ino, const struct efs3_inode *in) {
    u64 pos = (u64)ino * INODE_SIZE;
    u64 block = fs->sb.inode_table + pos / BLOCK_SIZE;
    u8 *b = block_read(fs->dev, block);
    if (!b) return false;
    memcpy(b + pos % BLOCK_SIZE, in, INODE_SIZE);
    return block_write(fs->dev, block, b);
}

/* Finds a clear bit in a bitmap spread over consecutive blocks, sets it and returns it (0 = none). */
static u32 bitmap_alloc(struct efs3 *fs, u32 first_block, u32 nbits, u32 *hint) {
    const u32 bits_per_block = BLOCK_SIZE * 8;
    for (u32 k = 0; k < nbits; k += 8) {
        u32 bit = ((*hint + k) % nbits) & ~7u;
        u64 block = first_block + bit / bits_per_block;
        u8 *b = block_read(fs->dev, block);
        if (!b) return 0;
        u32 byte = (bit % bits_per_block) / 8;
        if (b[byte] == 0xFF) continue;
        u32 i = (u32)__builtin_ctz(~(u32)b[byte] & 0xFF);
        if (bit + i >= nbits) continue;
        b[byte] |= (u8)(1 << i);
        if (!block_write(fs->dev, block, b)) return 0;
        *hint = bit + i;
        return bit + i;
    }
    return 0;
}

static void bitmap_clear(struct efs3 *fs, u32 first_block, u32 bit) {
    u64 block = first_block + bit / (BLOCK_SIZE * 8);
    u8 *b = block_read(fs->dev, block);
    if (!b) return;
    b[(bit % (BLOCK_SIZE * 8)) / 8] &= (u8)~(1 << (bit % 8));
    block_write(fs->dev, block, b);
}

static u32 alloc_block(struct efs3 *fs) {
    if (!fs->sb.free_blocks) return 0;
    u32 b = bitmap_alloc(fs, fs->sb.block_bitmap, fs->sb.block_count, &fs->block_hint);
    if (!b) return 0;
    if (b < fs->sb.first_data) panic("efs3: block bitmap hands out metadata block %u", b);
    fs->sb.free_blocks--;
    write_super(fs);
    block_write(fs->dev, b, zero_block);
    return b;
}

static void free_block(struct efs3 *fs, u32 b) {
    if (!b) return;
    bitmap_clear(fs, fs->sb.block_bitmap, b);
    fs->sb.free_blocks++;
    write_super(fs);
}

static u32 alloc_inode(struct efs3 *fs) {
    if (!fs->sb.free_inodes) return 0;
    u32 n = bitmap_alloc(fs, fs->sb.inode_bitmap, fs->sb.inode_count, &fs->inode_hint);
    if (!n) return 0;
    fs->sb.free_inodes--;
    write_super(fs);
    return n;
}

static void free_inode(struct efs3 *fs, u32 n) {
    bitmap_clear(fs, fs->sb.inode_bitmap, n);
    fs->sb.free_inodes++;
    write_super(fs);
}

/* ---- block mapping ----------------------------------------------------------- */

/* Reads entry i of an indirect block; with `alloc`, allocates a block for an empty entry. */
static u32 indirect_entry(struct efs3 *fs, u32 table, u32 i, bool alloc) {
    u8 *b = block_read(fs->dev, table);
    if (!b) return 0;
    u32 v = ((u32 *)b)[i];
    if (v || !alloc) return v;
    v = alloc_block(fs);   /* invalidates b */
    if (!v) return 0;
    b = block_read(fs->dev, table);
    if (!b) return 0;
    ((u32 *)b)[i] = v;
    block_write(fs->dev, table, b);
    return v;
}

/* Disk block of file block `fb` (0 = hole). With `alloc`, fills holes; the caller saves the inode. */
static u32 bmap(struct efs3 *fs, struct efs3_inode *in, u64 fb, bool alloc) {
    if (fb < 12) {
        if (!in->direct[fb] && alloc) in->direct[fb] = alloc_block(fs);
        return in->direct[fb];
    }
    fb -= 12;
    if (fb < PTRS) {
        if (!in->indirect && (!alloc || !(in->indirect = alloc_block(fs)))) return 0;
        return indirect_entry(fs, in->indirect, (u32)fb, alloc);
    }
    fb -= PTRS;
    if (fb >= (u64)PTRS * PTRS) return 0;
    if (!in->dindirect && (!alloc || !(in->dindirect = alloc_block(fs)))) return 0;
    u32 inner = indirect_entry(fs, in->dindirect, (u32)(fb / PTRS), alloc);
    return inner ? indirect_entry(fs, inner, (u32)(fb % PTRS), alloc) : 0;
}

/* Frees every data block from file block `first` on (and indirect blocks that become unused). */
static void free_from(struct efs3 *fs, struct efs3_inode *in, u64 first) {
    for (u64 fb = first; fb < 12; fb++) {
        free_block(fs, in->direct[fb]);
        in->direct[fb] = 0;
    }
    if (in->indirect) {
        u64 start = first > 12 ? first - 12 : 0;
        for (u64 i = start; i < PTRS; i++) {
            u8 *b = block_read(fs->dev, in->indirect);
            u32 v = b ? ((u32 *)b)[i] : 0;
            if (!v) continue;
            ((u32 *)b)[i] = 0;
            block_write(fs->dev, in->indirect, b);
            free_block(fs, v);
        }
        if (start == 0) {
            free_block(fs, in->indirect);
            in->indirect = 0;
        }
    }
    if (in->dindirect) {
        u64 start = first > 12 + PTRS ? first - 12 - PTRS : 0;
        for (u64 j = start / PTRS; j < PTRS; j++) {
            u8 *outer = block_read(fs->dev, in->dindirect);
            u32 inner = outer ? ((u32 *)outer)[j] : 0;
            if (!inner) continue;
            u64 from = j == start / PTRS ? start % PTRS : 0;
            for (u64 i = from; i < PTRS; i++) {
                u8 *b = block_read(fs->dev, inner);
                u32 v = b ? ((u32 *)b)[i] : 0;
                if (!v) continue;
                ((u32 *)b)[i] = 0;
                block_write(fs->dev, inner, b);
                free_block(fs, v);
            }
            if (from == 0) {
                outer = block_read(fs->dev, in->dindirect);
                if (outer) {
                    ((u32 *)outer)[j] = 0;
                    block_write(fs->dev, in->dindirect, outer);
                }
                free_block(fs, inner);
            }
        }
        if (start == 0) {
            free_block(fs, in->dindirect);
            in->dindirect = 0;
        }
    }
}

/* ---- file data ---------------------------------------------------------------- */

static i64 data_read(struct efs3 *fs, const struct efs3_inode *in, u64 off, u8 *buf, u64 len) {
    if (off >= in->size) return 0;
    len = MIN(len, in->size - off);
    struct efs3_inode copy = *in;
    for (u64 done = 0; done < len;) {
        u64 fb = (off + done) / BLOCK_SIZE, within = (off + done) % BLOCK_SIZE;
        u64 n = MIN(len - done, BLOCK_SIZE - within);
        u32 b = bmap(fs, &copy, fb, false);
        if (!b) {
            memset(buf + done, 0, n);
        } else {
            u8 *data = block_read(fs->dev, b);
            if (!data) return done ? (i64)done : -EIO;
            memcpy(buf + done, data + within, n);
        }
        done += n;
    }
    return (i64)len;
}

/* Writes and updates in->size; the caller saves the inode. */
static i64 data_write(struct efs3 *fs, struct efs3_inode *in, u64 off, const u8 *buf, u64 len) {
    u64 done = 0;
    while (done < len) {
        u64 fb = (off + done) / BLOCK_SIZE, within = (off + done) % BLOCK_SIZE;
        u64 n = MIN(len - done, BLOCK_SIZE - within);
        u32 b = bmap(fs, in, fb, true);
        if (!b) break;   /* disk full */
        if (n == BLOCK_SIZE) {
            if (!block_write(fs->dev, b, buf + done)) break;
        } else {
            u8 *data = block_read(fs->dev, b);
            if (!data) break;
            memcpy(data + within, buf + done, n);
            if (!block_write(fs->dev, b, data)) break;
        }
        done += n;
        if (off + done > in->size) in->size = off + done;
    }
    if (done == 0 && len) return -ENOSPC;
    return (i64)done;
}

static void set_size(struct efs3 *fs, struct efs3_inode *in, u64 size) {
    if (size < in->size) {
        free_from(fs, in, ALIGN_UP(size, BLOCK_SIZE) / BLOCK_SIZE);
        if (size % BLOCK_SIZE) {   /* zero the rest of the last block, so growing later shows zeros */
            u32 b = bmap(fs, in, size / BLOCK_SIZE, false);
            u8 *data = b ? block_read(fs->dev, b) : NULL;
            if (data) {
                memset(data + size % BLOCK_SIZE, 0, BLOCK_SIZE - size % BLOCK_SIZE);
                block_write(fs->dev, b, data);
            }
        }
    }
    in->size = size;
}

/* ---- directories -------------------------------------------------------------- */

/* Calls `fn` for each record; stops when it returns true. Returns false on a corrupt directory. */
typedef bool (*record_fn)(struct dirent_raw *r, u32 block, u32 offset, void *ctx);

static bool dir_iterate(struct efs3 *fs, struct efs3_inode *dir, record_fn fn, void *ctx, bool *stopped) {
    *stopped = false;
    for (u64 fb = 0; fb < dir->size / BLOCK_SIZE; fb++) {
        u32 b = bmap(fs, dir, fb, false);
        u8 *data = b ? block_read(fs->dev, b) : NULL;
        if (!data) return false;
        for (u32 off = 0; off < BLOCK_SIZE;) {
            struct dirent_raw *r = (struct dirent_raw *)(data + off);
            if (r->rec_len < 8 || r->rec_len % 4 || off + r->rec_len > BLOCK_SIZE || 8u + r->name_len > r->rec_len)
                return false;
            if (fn(r, b, off, ctx)) {
                *stopped = true;
                return true;
            }
            data = block_read(fs->dev, b);   /* fn may have used the cache */
            if (!data) return false;
            off += ((struct dirent_raw *)(data + off))->rec_len;
        }
    }
    return true;
}

struct find_ctx {
    const char *name;
    u32 len;
    u32 ino, block, offset;
    u8 type;
};

static bool find_fn(struct dirent_raw *r, u32 block, u32 offset, void *p) {
    struct find_ctx *c = p;
    if (!r->ino || r->name_len != c->len || memcmp(r->name, c->name, c->len)) return false;
    c->ino = r->ino;
    c->type = r->type;
    c->block = block;
    c->offset = offset;
    return true;
}

/* 1 = found, 0 = not found, <0 = error */
static int dir_find(struct efs3 *fs, struct efs3_inode *dir, const char *name, u32 len, struct find_ctx *c) {
    c->name = name;
    c->len = len;
    bool stopped;
    if (!dir_iterate(fs, dir, find_fn, c, &stopped)) return -EIO;
    return stopped ? 1 : 0;
}

static u16 record_size(u32 name_len) { return (u16)((8 + name_len + 3) & ~3u); }

struct add_ctx {
    struct efs3 *fs;
    const char *name;
    u32 len, ino;
    u8 type;
};

static bool add_fn(struct dirent_raw *r, u32 block, u32 offset, void *p) {
    struct add_ctx *c = p;
    u16 need = record_size(c->len);
    u16 used = r->ino ? record_size(r->name_len) : 0;
    if (r->rec_len - used < need) return false;
    u8 *data = block_read(c->fs->dev, block);
    if (!data) return false;
    r = (struct dirent_raw *)(data + offset);
    struct dirent_raw *n = r;
    if (used) {   /* split this record: it keeps `used` bytes, the new one gets the rest */
        n = (struct dirent_raw *)((u8 *)r + used);
        n->rec_len = r->rec_len - used;
        r->rec_len = used;
    }
    n->ino = c->ino;
    n->name_len = (u8)c->len;
    n->type = c->type;
    memcpy(n->name, c->name, c->len);
    block_write(c->fs->dev, block, data);
    return true;
}

static int dir_add(struct efs3 *fs, u32 dir_ino, struct efs3_inode *dir, const char *name, u32 len, u32 ino, u8 type) {
    struct add_ctx c = {fs, name, len, ino, type};
    bool stopped;
    if (!dir_iterate(fs, dir, add_fn, &c, &stopped)) return -EIO;
    if (stopped) return 0;
    /* no room: append a block holding one record that covers it */
    u64 fb = dir->size / BLOCK_SIZE;
    u32 b = bmap(fs, dir, fb, true);
    if (!b) return -ENOSPC;
    u8 *data = block_read(fs->dev, b);
    if (!data) return -EIO;
    struct dirent_raw *r = (struct dirent_raw *)data;
    r->ino = ino;
    r->rec_len = BLOCK_SIZE;
    r->name_len = (u8)len;
    r->type = type;
    memcpy(r->name, name, len);
    block_write(fs->dev, b, data);
    dir->size += BLOCK_SIZE;
    return inode_put(fs, dir_ino, dir) ? 0 : -EIO;
}

struct remove_ctx {
    struct efs3 *fs;
    const char *name;
    u32 len;
    u32 prev_block, prev_offset;
    bool have_prev;
};

static bool remove_fn(struct dirent_raw *r, u32 block, u32 offset, void *p) {
    struct remove_ctx *c = p;
    if (offset == 0) c->have_prev = false;
    if (!r->ino || r->name_len != c->len || memcmp(r->name, c->name, c->len)) {
        c->have_prev = true;
        c->prev_block = block;
        c->prev_offset = offset;
        return false;
    }
    u8 *data = block_read(c->fs->dev, block);
    if (!data) return true;
    r = (struct dirent_raw *)(data + offset);
    if (c->have_prev && c->prev_block == block) {   /* merge into the previous record */
        ((struct dirent_raw *)(data + c->prev_offset))->rec_len += r->rec_len;
    } else {
        r->ino = 0;
    }
    block_write(c->fs->dev, block, data);
    return true;
}

static int dir_remove(struct efs3 *fs, struct efs3_inode *dir, const char *name, u32 len) {
    struct remove_ctx c = {fs, name, len, 0, 0, false};
    bool stopped;
    if (!dir_iterate(fs, dir, remove_fn, &c, &stopped)) return -EIO;
    return stopped ? 0 : -ENOENT;
}

static bool nonempty_fn(struct dirent_raw *r, u32 block, u32 offset, void *p) {
    (void)block, (void)offset, (void)p;
    if (!r->ino) return false;
    bool dot = (r->name_len == 1 && r->name[0] == '.') || (r->name_len == 2 && r->name[0] == '.' && r->name[1] == '.');
    return !dot;
}

static int dir_is_empty(struct efs3 *fs, struct efs3_inode *dir) {
    bool stopped;
    if (!dir_iterate(fs, dir, nonempty_fn, NULL, &stopped)) return -EIO;
    return stopped ? 0 : 1;
}

/* ---- vnodes -------------------------------------------------------------------- */

static struct vnode *get_vnode(struct efs3 *fs, u32 ino) {
    for (struct vnode *v = fs->cache; v; v = v->cache_next)
        if (v->ino == ino) {
            vnode_ref(v);
            return v;
        }
    struct efs3_inode in;
    if (!inode_get(fs, ino, &in)) return NULL;
    struct vnode *v = kcalloc(1, sizeof *v);
    if (!v) return NULL;
    v->type = (in.mode & S_IFMT) == S_IFDIR ? VN_DIR : VN_FILE;
    v->refs = 1;
    v->ino = ino;
    v->ops = &efs3_ops;
    v->fs = fs;
    v->cache_next = fs->cache;
    fs->cache = v;
    return v;
}

static bool is_open(struct efs3 *fs, u32 ino) {
    for (struct vnode *v = fs->cache; v; v = v->cache_next)
        if (v->ino == ino && __atomic_load_n(&v->refs, __ATOMIC_ACQUIRE) > 0) return true;
    return false;
}

static void destroy_inode(struct efs3 *fs, u32 ino, struct efs3_inode *in) {
    free_from(fs, in, 0);
    memset(in, 0, sizeof *in);
    inode_put(fs, ino, in);
    free_inode(fs, ino);
}

static void efs3_release(struct vnode *v) {
    struct efs3 *fs = v->fs;
    mutex_lock(&fs->lock);
    if (__atomic_load_n(&v->refs, __ATOMIC_ACQUIRE) > 0) {   /* looked up again meanwhile */
        mutex_unlock(&fs->lock);
        return;
    }
    for (struct vnode **p = &fs->cache; *p; p = &(*p)->cache_next)
        if (*p == v) {
            *p = v->cache_next;
            break;
        }
    struct efs3_inode in;
    if (inode_get(fs, (u32)v->ino, &in) && in.links == 0) destroy_inode(fs, (u32)v->ino, &in);   /* deleted while open */
    mutex_unlock(&fs->lock);
    kfree(v);
}

/* ---- operations ---------------------------------------------------------------- */

#define LOCKED(fs, expr)              \
    ({                                \
        mutex_lock(&(fs)->lock);      \
        __typeof__(expr) _r = (expr); \
        mutex_unlock(&(fs)->lock);    \
        _r;                           \
    })

static i64 do_read(struct efs3 *fs, struct vnode *v, u64 off, void *buf, u64 len) {
    struct efs3_inode in;
    if (!inode_get(fs, (u32)v->ino, &in)) return -EIO;
    return data_read(fs, &in, off, buf, len);
}

static i64 efs3_read(struct vnode *v, u64 off, void *buf, u64 len) {
    struct efs3 *fs = v->fs;
    if (v->type == VN_DIR) return -EISDIR;
    return LOCKED(fs, do_read(fs, v, off, buf, len));
}

static i64 do_write(struct efs3 *fs, struct vnode *v, u64 off, const void *buf, u64 len) {
    struct efs3_inode in;
    if (!inode_get(fs, (u32)v->ino, &in)) return -EIO;
    i64 r = data_write(fs, &in, off, buf, len);
    if (!inode_put(fs, (u32)v->ino, &in)) return -EIO;
    return r;
}

static i64 efs3_write(struct vnode *v, u64 off, const void *buf, u64 len) {
    struct efs3 *fs = v->fs;
    if (v->type == VN_DIR) return -EISDIR;
    return LOCKED(fs, do_write(fs, v, off, buf, len));
}

static int do_truncate(struct efs3 *fs, struct vnode *v, u64 size) {
    struct efs3_inode in;
    if (!inode_get(fs, (u32)v->ino, &in)) return -EIO;
    set_size(fs, &in, size);
    return inode_put(fs, (u32)v->ino, &in) ? 0 : -EIO;
}

static int efs3_truncate(struct vnode *v, u64 size) {
    struct efs3 *fs = v->fs;
    if (v->type == VN_DIR) return -EISDIR;
    return LOCKED(fs, do_truncate(fs, v, size));
}

static int do_lookup(struct efs3 *fs, struct vnode *dir, const char *name, u32 len, struct vnode **out) {
    struct efs3_inode d;
    if (!inode_get(fs, (u32)dir->ino, &d)) return -EIO;
    struct find_ctx c;
    int r = dir_find(fs, &d, name, len, &c);
    if (r <= 0) return r ? r : -ENOENT;
    *out = get_vnode(fs, c.ino);
    return *out ? 0 : -EIO;
}

static int efs3_lookup(struct vnode *dir, const char *name, u32 len, struct vnode **out) {
    struct efs3 *fs = dir->fs;
    return LOCKED(fs, do_lookup(fs, dir, name, len, out));
}

static int do_create(struct efs3 *fs, struct vnode *dir, const char *name, u32 len, int type, struct vnode **out) {
    struct efs3_inode d;
    if (!inode_get(fs, (u32)dir->ino, &d)) return -EIO;
    if (d.links == 0) return -ENOENT;   /* the directory was deleted */
    struct find_ctx c;
    int r = dir_find(fs, &d, name, len, &c);
    if (r < 0) return r;
    if (r > 0) return -EEXIST;

    u32 ino = alloc_inode(fs);
    if (!ino) return -ENOSPC;
    struct efs3_inode in;
    memset(&in, 0, sizeof in);
    if (type == VN_DIR) {
        in.mode = S_IFDIR | 0755;
        in.links = 2;
        u32 b = bmap(fs, &in, 0, true);
        u8 *data = b ? block_read(fs->dev, b) : NULL;
        if (!data) {
            free_inode(fs, ino);
            return -ENOSPC;
        }
        struct dirent_raw *dot = (struct dirent_raw *)data;
        *dot = (struct dirent_raw){(u32)ino, 12, 1, FT_DIR};
        dot->name[0] = '.';
        struct dirent_raw *dotdot = (struct dirent_raw *)(data + 12);
        *dotdot = (struct dirent_raw){(u32)dir->ino, BLOCK_SIZE - 12, 2, FT_DIR};
        dotdot->name[0] = dotdot->name[1] = '.';
        block_write(fs->dev, b, data);
        in.size = BLOCK_SIZE;
    } else {
        in.mode = S_IFREG | 0644;
        in.links = 1;
    }
    if (!inode_put(fs, ino, &in)) return -EIO;
    if (!inode_get(fs, (u32)dir->ino, &d)) return -EIO;   /* re-read: allocation touched the cache */
    r = dir_add(fs, (u32)dir->ino, &d, name, len, ino, type == VN_DIR ? FT_DIR : FT_FILE);
    if (r < 0) {
        destroy_inode(fs, ino, &in);
        return r;
    }
    if (type == VN_DIR) {
        if (!inode_get(fs, (u32)dir->ino, &d)) return -EIO;
        d.links++;
        inode_put(fs, (u32)dir->ino, &d);
    }
    *out = get_vnode(fs, ino);
    return *out ? 0 : -EIO;
}

static int efs3_create(struct vnode *dir, const char *name, u32 len, int type, struct vnode **out) {
    struct efs3 *fs = dir->fs;
    return LOCKED(fs, do_create(fs, dir, name, len, type, out));
}

/* Drops one link of `ino`; frees it once unused (now, or when the last open file closes). */
static void unlink_inode(struct efs3 *fs, u32 ino, struct efs3_inode *in, bool is_dir) {
    in->links = is_dir ? 0 : in->links - 1;
    inode_put(fs, ino, in);
    if (in->links == 0 && !is_open(fs, ino)) destroy_inode(fs, ino, in);
}

static int do_remove(struct efs3 *fs, struct vnode *dir, const char *name, u32 len, bool is_dir) {
    struct efs3_inode d, t;
    if (!inode_get(fs, (u32)dir->ino, &d)) return -EIO;
    struct find_ctx c;
    int r = dir_find(fs, &d, name, len, &c);
    if (r <= 0) return r ? r : -ENOENT;
    if (!inode_get(fs, c.ino, &t)) return -EIO;
    bool target_dir = (t.mode & S_IFMT) == S_IFDIR;
    if (is_dir && !target_dir) return -ENOTDIR;
    if (!is_dir && target_dir) return -EISDIR;
    if (target_dir) {
        r = dir_is_empty(fs, &t);
        if (r <= 0) return r ? r : -ENOTEMPTY;
    }
    r = dir_remove(fs, &d, name, len);
    if (r < 0) return r;
    unlink_inode(fs, c.ino, &t, target_dir);
    if (target_dir) {
        if (!inode_get(fs, (u32)dir->ino, &d)) return -EIO;
        d.links--;
        inode_put(fs, (u32)dir->ino, &d);
    }
    return 0;
}

static int efs3_remove(struct vnode *dir, const char *name, u32 len, bool is_dir) {
    struct efs3 *fs = dir->fs;
    return LOCKED(fs, do_remove(fs, dir, name, len, is_dir));
}

/* True if directory `ino` is `ancestor` or lies inside it (follows ".." up to the root). */
static bool inside(struct efs3 *fs, u32 ino, u32 ancestor) {
    for (int depth = 0; depth < 256; depth++) {
        if (ino == ancestor) return true;
        if (ino == fs->sb.root_inode) return false;
        struct efs3_inode d;
        struct find_ctx c;
        if (!inode_get(fs, ino, &d) || dir_find(fs, &d, "..", 2, &c) <= 0) return false;
        ino = c.ino;
    }
    return true;   /* too deep or a loop: refuse */
}

static int do_rename(struct efs3 *fs, struct vnode *od, const char *on, u32 ol, struct vnode *nd, const char *nn, u32 nl) {
    struct efs3_inode odir, ndir, src;
    if (!inode_get(fs, (u32)od->ino, &odir) || !inode_get(fs, (u32)nd->ino, &ndir)) return -EIO;
    struct find_ctx s, t;
    int r = dir_find(fs, &odir, on, ol, &s);
    if (r <= 0) return r ? r : -ENOENT;
    if (!inode_get(fs, s.ino, &src)) return -EIO;
    bool src_dir = (src.mode & S_IFMT) == S_IFDIR;
    if (src_dir && inside(fs, (u32)nd->ino, s.ino)) return -EINVAL;   /* into itself */

    r = dir_find(fs, &ndir, nn, nl, &t);
    if (r < 0) return r;
    if (r > 0) {
        if (t.ino == s.ino) return 0;
        struct efs3_inode target;
        if (!inode_get(fs, t.ino, &target)) return -EIO;
        bool target_dir = (target.mode & S_IFMT) == S_IFDIR;
        if (src_dir || target_dir) return src_dir && target_dir ? -EEXIST : target_dir ? -EISDIR : -ENOTDIR;
        r = dir_remove(fs, &ndir, nn, nl);   /* replace the existing file */
        if (r < 0) return r;
        unlink_inode(fs, t.ino, &target, false);
        if (!inode_get(fs, (u32)nd->ino, &ndir)) return -EIO;
    }
    r = dir_add(fs, (u32)nd->ino, &ndir, nn, nl, s.ino, src_dir ? FT_DIR : FT_FILE);
    if (r < 0) return r;
    if (!inode_get(fs, (u32)od->ino, &odir)) return -EIO;
    r = dir_remove(fs, &odir, on, ol);
    if (r < 0) return r;
    if (src_dir && od->ino != nd->ino) {   /* ".." now points to the new parent */
        if (!inode_get(fs, s.ino, &src)) return -EIO;
        dir_remove(fs, &src, "..", 2);
        if (!inode_get(fs, s.ino, &src)) return -EIO;
        dir_add(fs, s.ino, &src, "..", 2, (u32)nd->ino, FT_DIR);
        if (inode_get(fs, (u32)od->ino, &odir)) {
            odir.links--;
            inode_put(fs, (u32)od->ino, &odir);
        }
        if (inode_get(fs, (u32)nd->ino, &ndir)) {
            ndir.links++;
            inode_put(fs, (u32)nd->ino, &ndir);
        }
    }
    return 0;
}

static int efs3_rename(struct vnode *od, const char *on, u32 ol, struct vnode *nd, const char *nn, u32 nl) {
    struct efs3 *fs = od->fs;
    if (nd->fs != fs) return -EXDEV;
    return LOCKED(fs, do_rename(fs, od, on, ol, nd, nn, nl));
}

struct readdir_ctx {
    u64 skip_until;        /* byte position (block * 4096 + offset) to resume from */
    u64 pos;
    struct dir_entry *out;
    u64 next;
};

static int do_readdir(struct efs3 *fs, struct vnode *dir, u64 *cookie, struct dir_entry *out) {
    struct efs3_inode d;
    if (!inode_get(fs, (u32)dir->ino, &d)) return -EIO;
    u64 pos = *cookie;
    while (pos < d.size) {
        u64 fb = pos / BLOCK_SIZE;
        u32 off = (u32)(pos % BLOCK_SIZE);
        u32 b = bmap(fs, &d, fb, false);
        u8 *data = b ? block_read(fs->dev, b) : NULL;
        if (!data) return -EIO;
        struct dirent_raw *r = (struct dirent_raw *)(data + off);
        if (r->rec_len < 8 || r->rec_len % 4 || off + r->rec_len > BLOCK_SIZE) return -EIO;
        pos += r->rec_len;
        if (!r->ino) continue;
        out->ino = r->ino;
        out->type = r->type == FT_DIR ? VN_DIR : VN_FILE;
        out->name_len = r->name_len;
        memcpy(out->name, r->name, r->name_len);
        out->name[r->name_len] = 0;
        *cookie = pos;
        return 1;
    }
    *cookie = pos;
    return 0;
}

static int efs3_readdir(struct vnode *dir, u64 *cookie, struct dir_entry *out) {
    struct efs3 *fs = dir->fs;
    return LOCKED(fs, do_readdir(fs, dir, cookie, out));
}

static int do_stat(struct efs3 *fs, struct vnode *v, struct vnode_stat *st) {
    struct efs3_inode in;
    if (!inode_get(fs, (u32)v->ino, &in)) return -EIO;
    st->ino = v->ino;
    st->mode = in.mode;
    st->links = in.links;
    st->size = in.size;
    st->blocks = ALIGN_UP(in.size, BLOCK_SIZE) / 512;
    st->dev = 1;
    st->rdev = 0;
    return 0;
}

static int efs3_stat(struct vnode *v, struct vnode_stat *st) {
    struct efs3 *fs = v->fs;
    return LOCKED(fs, do_stat(fs, v, st));
}

static int efs3_sync(struct vnode *v) {
    struct efs3 *fs = v->fs;
    return LOCKED(fs, block_flush(fs->dev) ? 0 : -EIO);
}

static const struct vnode_ops efs3_ops = {
    .read = efs3_read,
    .write = efs3_write,
    .truncate = efs3_truncate,
    .lookup = efs3_lookup,
    .create = efs3_create,
    .remove = efs3_remove,
    .rename = efs3_rename,
    .readdir = efs3_readdir,
    .stat = efs3_stat,
    .sync = efs3_sync,
    .release = efs3_release,
};

int efs3_mount(struct block_device *dev, struct vnode **root) {
    u8 *b = block_read(dev, 0);
    if (!b) return -EIO;
    struct efs3 *fs = kcalloc(1, sizeof *fs);
    if (!fs) return -ENOMEM;
    memcpy(&fs->sb, b, sizeof fs->sb);
    if (memcmp(fs->sb.magic, "EFS3", 4) || fs->sb.version != 1 || fs->sb.block_size != BLOCK_SIZE ||
        (u64)fs->sb.block_count * SECTORS_PER_BLOCK > dev->sector_count) {
        kfree(fs);
        return -EINVAL;
    }
    fs->dev = dev;
    fs->block_hint = fs->sb.first_data;
    fs->inode_hint = 1;
    mutex_lock(&fs->lock);
    *root = get_vnode(fs, fs->sb.root_inode);
    mutex_unlock(&fs->lock);
    if (!*root) {
        kfree(fs);
        return -EIO;
    }
    return 0;
}

void efs3_info(struct vnode *root, u64 *total, u64 *free_blocks, char *label, u32 label_size) {
    struct efs3 *fs = root->fs;
    *total = fs->sb.block_count;
    *free_blocks = fs->sb.free_blocks;
    u32 n = 0;
    while (n < 32 && n + 1 < label_size && fs->sb.label[n]) {
        label[n] = fs->sb.label[n];
        n++;
    }
    label[n] = 0;
}
