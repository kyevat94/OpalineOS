# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""EFS3, the OpalineOS file system: creates images from a folder and reads them back.

    python tools/efs3.py ls   DISK.img [PATH]     # list a directory of the root partition
    python tools/efs3.py cat  DISK.img PATH       # print a file
    python tools/efs3.py tree DISK.img            # whole tree with sizes
    python tools/efs3.py check DISK.img           # consistency check (bitmaps, link counts, journal)

Other tools import make_fs() (build) and Efs3 (read).

On-disk format (little endian, 4 KiB blocks; kernel/fs/efs3.c is the other implementation):

    block 0              superblock
    block 1              inode bitmap (bit n = inode n in use; inode 0 is never used)
    block 2..            block bitmap (bit n = block n in use, metadata included)
    then                 inode table (128-byte inodes, inode n at byte n * 128)
    then                 journal (version 2: JOURNAL_BLOCKS blocks)
    then                 data blocks

    superblock:  "EFS3", version, block_size, block_count, inode_count, inode_bitmap_block,
                 block_bitmap_block, block_bitmap_blocks, inode_table_block, inode_table_blocks,
                 first_data_block, root_inode (1), free_blocks, free_inodes, reserved,
                 label[32] (UTF-8), uuid[16], journal_start, journal_blocks (version 2)
    inode:       mode u16 (0x4000 dir / 0x8000 file | permissions, set-id and sticky bits),
                 links u16, flags u32, size u64, ctime u64, mtime u64, direct[12] u32,
                 indirect u32, double_indirect u32, uid u32, gid u32, atime u64,
                 24 reserved bytes  (block number 0 = hole/none; times in nanoseconds since
                 1970 UTC; images from before stage SE1 have zero there: root, 1970)
    directory:   records that cover every byte of each data block:
                 inode u32 (0 = free), rec_len u16 (multiple of 4), name_len u8,
                 type u8 (1 file, 2 directory, 3 symbolic link), name (UTF-8, NFC, no '/' or NUL,
                 1..255 bytes). Several entries may name the same file (hard links).
    symbolic link: an inode with mode 0xA000 | 0777 whose first data block holds the target path
                 (size = its length, at most 4095 bytes)
    journal:     block 0 header ("EFJH", version u32, seq u64: the next transaction's number),
                 block 1 descriptor ("EFJD", count u32, seq u64, block numbers u32[count]),
                 blocks 2.. copies of those blocks, then a commit block ("EFJC", count u32, seq u64,
                 checksum u32 = FNV-1a over count, seq, the block list and every copy). A committed
                 transaction with seq >= the header's is written in place again at mount.

Names are stored NFC-normalized, so "ação" typed with combining accents is the same file.
"""
import os
import struct
import sys
import time
import unicodedata
import uuid

BLOCK = 4096
INODE_SIZE = 128
PTRS = BLOCK // 4                    # block numbers per indirect block
MAGIC = b"EFS3"
ROOT_INODE = 1
S_IFDIR, S_IFREG = 0x4000, 0x8000
FT_FILE, FT_DIR, FT_SYMLINK = 1, 2, 3
S_IFLNK = 0xA000
JOURNAL_BLOCKS = 256

SUPER_FMT = "<4s14I32s16sII"
INODE_FMT = "<HHIQQQ12IIIIIQ24s"
assert struct.calcsize(INODE_FMT) == INODE_SIZE

# GPT partition type of the OpalineOS root partition
ROOT_TYPE_GUID = uuid.UUID("4f50414c-494e-4552-4f4f-542d45465333")


def nfc(name):
    n = unicodedata.normalize("NFC", name)
    raw = n.encode("utf-8")
    if not raw or len(raw) > 255 or "/" in n or "\0" in n or n in (".", ".."):
        raise ValueError(f"invalid file name: {name!r}")
    return raw


def layout(block_count):
    inode_count = min(32768, max(64, block_count // 4))
    inode_count = (inode_count + 31) // 32 * 32
    bbmap_blocks = (block_count + BLOCK * 8 - 1) // (BLOCK * 8)
    itable_blocks = inode_count * INODE_SIZE // BLOCK
    itable = 2 + bbmap_blocks
    jstart = itable + itable_blocks
    jblocks = min(JOURNAL_BLOCKS, max(16, block_count // 16))
    return inode_count, bbmap_blocks, itable, itable_blocks, jstart, jblocks, jstart + jblocks


def fnv1a(h, data):
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


def dir_records(entries):
    """Packs (name_bytes, inode, type) into directory blocks."""
    blocks, cur = [], bytearray()
    for name, ino, ftype in entries:
        need = (8 + len(name) + 3) & ~3
        if len(cur) + need > BLOCK:
            blocks.append(cur)
            cur = bytearray()
        cur += struct.pack("<IHBB", ino, need, len(name), ftype) + name + b"\0" * (need - 8 - len(name))
    blocks.append(cur)
    out = []
    for b in blocks:   # the last record of each block takes the remaining space
        last = 0
        off = 0
        while off < len(b):
            last = off
            off += struct.unpack_from("<H", b, off + 4)[0]
        if b:
            struct.pack_into("<H", b, last + 4, BLOCK - last)
        else:
            b += struct.pack("<IHBB", 0, BLOCK, 0, 0)
        out.append(bytes(b) + b"\0" * (BLOCK - len(b)))
    return out


# Owner and mode of some folders of the OpalineOS root tree (path relative to the root): mode, uid, gid.
# Everything else belongs to root (0:0), with 0755 for folders and programs and 0644 for files.
SPECIAL = {
    "tmp": (0o1777, 0, 0),         # everyone may create files; only the owner deletes them (sticky)
    "root": (0o700, 0, 0),         # root's home
    "home/user": (0o755, 1000, 1000),
}


def make_fs(source_dir, size_bytes, label="OpalineOS", when_ns=None):
    """Builds an EFS3 image of `size_bytes` containing the files under `source_dir`.
    Every file gets the time `when_ns` (default: now)."""
    stamp = time.time_ns() if when_ns is None else when_ns
    block_count = size_bytes // BLOCK
    inode_count, bbmap_blocks, itable, itable_blocks, jstart, jblocks, first_data = layout(block_count)
    img = bytearray(block_count * BLOCK)
    inodes = {}            # number -> dict
    next_inode = [ROOT_INODE]
    next_block = [first_data]

    def alloc_inode():
        n = next_inode[0]
        if n >= inode_count:
            raise ValueError("file system full (inodes)")
        next_inode[0] += 1
        return n

    def alloc_block():
        b = next_block[0]
        if b >= block_count:
            raise ValueError("file system full (blocks)")
        next_block[0] += 1
        return b

    def write_data(data):
        """Stores data and returns (direct[12], indirect, double_indirect)."""
        nblocks = (len(data) + BLOCK - 1) // BLOCK
        blocks = []
        for i in range(nblocks):
            b = alloc_block()
            chunk = data[i * BLOCK:(i + 1) * BLOCK]
            img[b * BLOCK:b * BLOCK + len(chunk)] = chunk
            blocks.append(b)
        direct = blocks[:12] + [0] * (12 - min(12, len(blocks)))
        rest = blocks[12:]
        ind = dind = 0
        if rest:
            ind = alloc_block()
            first = rest[:PTRS]
            struct.pack_into(f"<{len(first)}I", img, ind * BLOCK, *first)
            rest = rest[PTRS:]
        if rest:
            dind = alloc_block()
            for k in range(0, len(rest), PTRS):
                part = rest[k:k + PTRS]
                ib = alloc_block()
                struct.pack_into(f"<{len(part)}I", img, ib * BLOCK, *part)
                struct.pack_into("<I", img, dind * BLOCK + (k // PTRS) * 4, ib)
        return direct, ind, dind

    def special(full, mode):
        rel = os.path.relpath(full, source_dir).replace(os.sep, "/")
        m, uid, gid = SPECIAL.get(rel, (mode, 0, 0))
        return dict(mode=(mode & 0o170000) | m, uid=uid, gid=gid)

    def add_dir(path, ino, parent):
        children = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            raw = nfc(name)
            child = alloc_inode()
            if os.path.isdir(full):
                children.append((raw, child, FT_DIR, full))
            else:
                with open(full, "rb") as f:
                    data = f.read()
                d, i, di = write_data(data)
                executable = data[:4] == b"\x7fELF"
                inodes[child] = dict(special(full, S_IFREG | (0o755 if executable else 0o644)), links=1,
                                     size=len(data), direct=d, ind=i, dind=di)
                children.append((raw, child, FT_FILE, None))
        entries = [(b".", ino, FT_DIR), (b"..", parent, FT_DIR)] + [(r, c, t) for r, c, t, _ in children]
        data = b"".join(dir_records(entries))
        d, i, di = write_data(data)
        subdirs = sum(1 for c in children if c[2] == FT_DIR)
        inodes[ino] = dict(special(path, S_IFDIR | 0o755), links=2 + subdirs, size=len(data), direct=d, ind=i, dind=di)
        for raw, child, ftype, full in children:
            if ftype == FT_DIR:
                add_dir(full, child, ino)

    root = alloc_inode()
    assert root == ROOT_INODE
    add_dir(source_dir, root, root)

    for n, ino in inodes.items():
        struct.pack_into(INODE_FMT, img, itable * BLOCK + n * INODE_SIZE, ino["mode"], ino["links"], 0,
                         ino["size"], stamp, stamp, *ino["direct"], ino["ind"], ino["dind"], ino["uid"], ino["gid"],
                         stamp, b"\0" * 24)
    used_inodes = next_inode[0]          # 0 .. used_inodes-1 are taken (0 is reserved)
    for n in range(used_inodes):
        img[BLOCK + n // 8] |= 1 << (n % 8)
    for b in range(next_block[0]):       # metadata + data are all below next_block
        img[2 * BLOCK + b // 8] |= 1 << (b % 8)
    for b in range(block_count, bbmap_blocks * BLOCK * 8):   # bits past the end: never free
        img[2 * BLOCK + b // 8] |= 1 << (b % 8)

    vol_uuid = uuid.uuid5(ROOT_TYPE_GUID, label).bytes
    struct.pack_into(SUPER_FMT, img, 0, MAGIC, 2, BLOCK, block_count, inode_count, 1, 2, bbmap_blocks, itable,
                     itable_blocks, first_data, ROOT_INODE, block_count - next_block[0],
                     inode_count - used_inodes, 0, label.encode("utf-8")[:32].ljust(32, b"\0"), vol_uuid,
                     jstart, jblocks)
    struct.pack_into("<4sIQ", img, jstart * BLOCK, b"EFJH", 1, 1)   # an empty journal
    return bytes(img)


class Efs3:
    """Reads an EFS3 file system (bytes of the partition)."""

    def __init__(self, data, replay=True):
        self.data = data
        self._super()
        self.pending = self.pending_transaction()
        self.replayed = None
        if self.pending and replay:   # what the kernel does at mount, done in memory
            seq, blocks, copies = self.pending
            buf = bytearray(self.data)
            for b, copy in zip(blocks, copies):
                buf[b * BLOCK:(b + 1) * BLOCK] = copy
            self.data = bytes(buf)
            self._super()
            self.replayed, self.pending = seq, None

    def _super(self):
        f = struct.unpack_from(SUPER_FMT, self.data, 0)
        if f[0] != MAGIC:
            raise ValueError("not an EFS3 file system")
        (_, self.version, bs, self.block_count, self.inode_count, _, _, _, self.itable, _, self.first_data, self.root,
         self.free_blocks, self.free_inodes, _, label, _, self.jstart, self.jblocks) = f
        if self.version < 2:
            self.jstart = self.jblocks = 0
        self.label = label.rstrip(b"\0").decode("utf-8")

    def pending_transaction(self):
        """A committed journal transaction not yet marked done: (seq, blocks, copies) or None."""
        if not self.jblocks:
            return None
        blk = lambda n: self.data[(self.jstart + n) * BLOCK:(self.jstart + n + 1) * BLOCK]
        magic, _, next_seq = struct.unpack_from("<4sIQ", blk(0))
        if magic != b"EFJH":
            next_seq = 1
        magic, count, seq = struct.unpack_from("<4sIQ", blk(1))
        if magic != b"EFJD" or not 0 < count <= self.jblocks - 3 or seq < next_seq:
            return None
        blocks = list(struct.unpack_from(f"<{count}I", blk(1), 16))
        cmagic, ccount, cseq, csum = struct.unpack_from("<4sIQI", blk(2 + count))
        copies = [blk(2 + i) for i in range(count)]
        h = fnv1a(fnv1a(fnv1a(2166136261, struct.pack("<I", count)), struct.pack("<Q", seq)),
                  struct.pack(f"<{count}I", *blocks))
        for c in copies:
            h = fnv1a(h, c)
        if cmagic != b"EFJC" or ccount != count or cseq != seq or csum != h:
            return None
        return seq, blocks, copies

    @classmethod
    def from_disk(cls, path, replay=True):
        """Opens the root partition of a GPT disk image."""
        with open(path, "rb") as f:
            disk = f.read()
        hdr = disk[512:512 + 92]
        entries_lba, count, size = struct.unpack_from("<QII", hdr, 72)
        for i in range(count):
            e = disk[entries_lba * 512 + i * size:entries_lba * 512 + (i + 1) * size]
            if uuid.UUID(bytes_le=e[:16]) == ROOT_TYPE_GUID:
                first, last = struct.unpack_from("<QQ", e, 32)
                return cls(disk[first * 512:(last + 1) * 512], replay)
        raise ValueError("no OpalineOS root partition in this disk image")

    def inode(self, n):
        v = struct.unpack_from(INODE_FMT, self.data, self.itable * BLOCK + n * INODE_SIZE)
        return dict(mode=v[0], links=v[1], size=v[3], ctime=v[4], mtime=v[5], direct=list(v[6:18]), ind=v[18],
                    dind=v[19], uid=v[20], gid=v[21], atime=v[22])

    def blocks(self, ino):
        nb = (ino["size"] + BLOCK - 1) // BLOCK
        out = ino["direct"][:min(12, nb)]
        if nb > 12 and ino["ind"]:
            out += list(struct.unpack_from(f"<{PTRS}I", self.data, ino["ind"] * BLOCK))[:nb - 12]
        if nb > 12 + PTRS and ino["dind"]:
            outer = struct.unpack_from(f"<{PTRS}I", self.data, ino["dind"] * BLOCK)
            for ib in outer:
                if len(out) >= nb:
                    break
                inner = struct.unpack_from(f"<{PTRS}I", self.data, ib * BLOCK) if ib else (0,) * PTRS
                out += list(inner)[:nb - len(out)]
        return out

    def read(self, n):
        ino = self.inode(n)
        buf = bytearray()
        for b in self.blocks(ino):
            buf += self.data[b * BLOCK:(b + 1) * BLOCK] if b else b"\0" * BLOCK
        return bytes(buf[:ino["size"]])

    def listdir(self, n):
        raw = self.read(n)
        out = []
        for base in range(0, len(raw), BLOCK):
            off = 0
            while off < BLOCK and base + off < len(raw):
                ino, rec, nlen, ftype = struct.unpack_from("<IHBB", raw, base + off)
                if rec == 0:
                    break
                if ino:
                    out.append((raw[base + off + 8:base + off + 8 + nlen].decode("utf-8"), ino, ftype))
                off += rec
        return out

    def lookup(self, path):
        n = self.root
        for part in [p for p in path.split("/") if p]:
            part = unicodedata.normalize("NFC", part)
            match = [ino for name, ino, _ in self.listdir(n) if name == part]
            if not match:
                raise FileNotFoundError(path)
            n = match[0]
        return n

    def check(self):
        """fsck: compares the bitmaps, link counts and free counts with what the directory tree really
        uses. A committed journal transaction that was not written in place is a problem too (the
        kernel replays it at mount; open the image with replay=False to see it)."""
        errors = []
        if self.pending:
            errors.append(f"journal: transaction {self.pending[0]} is committed but not written in place")
        used_blocks = set(range(self.first_data))
        used_inodes = {0}
        links = {}

        def mark_block(b, owner):
            if b in used_blocks:
                errors.append(f"block {b} used twice ({owner})")
            if not self.first_data <= b < self.block_count:
                errors.append(f"block {b} out of range ({owner})")
            used_blocks.add(b)

        def mark_blocks(n, ino, path):
            nb = (ino["size"] + BLOCK - 1) // BLOCK
            for b in self.blocks(ino):
                if b:
                    mark_block(b, path)
            if nb > 12 and ino["ind"]:
                mark_block(ino["ind"], path + " (indirect)")
            if ino["dind"]:
                mark_block(ino["dind"], path + " (double indirect)")
                outer = struct.unpack_from(f"<{PTRS}I", self.data, ino["dind"] * BLOCK)
                for ib in outer:
                    if ib:
                        mark_block(ib, path + " (indirect)")

        def visit_dir(n, path, parent):
            if n in used_inodes:
                errors.append(f"directory inode {n} reached twice ({path})")
                return
            used_inodes.add(n)
            ino = self.inode(n)
            mark_blocks(n, ino, path)
            entries = self.listdir(n)
            names = [e[0] for e in entries]
            if names[:2] != [".", ".."]:
                errors.append(f"{path}: '.' and '..' missing")
            elif entries[1][1] != parent:
                errors.append(f"{path}: '..' points to inode {entries[1][1]}, expected {parent}")
            subdirs = 0
            for name, child, ftype in entries[2:]:
                child_path = path.rstrip("/") + "/" + name
                kind = self.inode(child)["mode"] & 0xF000
                expected = FT_DIR if kind == S_IFDIR else FT_SYMLINK if kind == S_IFLNK else FT_FILE
                if ftype != expected:
                    errors.append(f"{child_path}: entry type {ftype}, inode type {expected}")
                if ftype == FT_DIR:
                    subdirs += 1
                    visit_dir(child, child_path, n)
                    continue
                links[child] = links.get(child, 0) + 1
                if child not in used_inodes:   # a file with several names is counted once
                    used_inodes.add(child)
                    cino = self.inode(child)
                    mark_blocks(child, cino, child_path)
                    if kind == S_IFLNK and not 0 < cino["size"] < BLOCK:
                        errors.append(f"{child_path}: symbolic link of {cino['size']} bytes")
            if ino["links"] != 2 + subdirs:
                errors.append(f"{path}: link count {ino['links']}, expected {2 + subdirs}")

        visit_dir(self.root, "/", self.root)
        for child, count in links.items():
            if self.inode(child)["links"] != count:
                errors.append(f"inode {child}: link count {self.inode(child)['links']}, {count} name(s) found")
        for b in range(self.block_count):
            in_bitmap = bool(self.data[2 * BLOCK + b // 8] & (1 << (b % 8)))
            if in_bitmap != (b in used_blocks):
                errors.append(f"block {b}: bitmap says {'used' if in_bitmap else 'free'}, tree says "
                              f"{'used' if b in used_blocks else 'free'}")
        for n in range(self.inode_count):
            in_bitmap = bool(self.data[BLOCK + n // 8] & (1 << (n % 8)))
            if in_bitmap != (n in used_inodes):
                errors.append(f"inode {n}: bitmap says {'used' if in_bitmap else 'free'}")
        if self.free_blocks != self.block_count - len(used_blocks):
            errors.append(f"superblock: {self.free_blocks} free blocks, counted {self.block_count - len(used_blocks)}")
        if self.free_inodes != self.inode_count - len(used_inodes):
            errors.append(f"superblock: {self.free_inodes} free inodes, counted {self.inode_count - len(used_inodes)}")
        return errors

    def readlink(self, n):
        return self.read(n).decode("utf-8", errors="replace")

    def walk(self, n=None, prefix=""):
        n = self.root if n is None else n
        for name, ino, ftype in self.listdir(n):
            if name in (".", ".."):
                continue
            yield prefix + "/" + name, ino, ftype
            if ftype == FT_DIR:
                yield from self.walk(ino, prefix + "/" + name)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, image = sys.argv[1], sys.argv[2]
    fs = Efs3.from_disk(image, replay=cmd != "check")
    sys.stdout.reconfigure(encoding="utf-8")
    if cmd == "ls":
        for name, ino, ftype in fs.listdir(fs.lookup(sys.argv[3] if len(sys.argv) > 3 else "/")):
            print(name + ("/" if ftype == FT_DIR else ""))
    elif cmd == "cat":
        sys.stdout.write(fs.read(fs.lookup(sys.argv[3])).decode("utf-8", errors="replace"))
    elif cmd == "check":
        errors = fs.check()
        for e in errors[:50]:
            print("ERROR:", e)
        print(f"{len(errors)} problem(s)" if errors else "OK: file system is consistent")
        sys.exit(1 if errors else 0)
    elif cmd == "tree":
        print(f"EFS3 \"{fs.label}\": {fs.block_count} blocks, {fs.free_blocks} free, {fs.free_inodes} inodes free")
        if fs.replayed:
            print(f"(journal transaction {fs.replayed} replayed in memory)")
        for path, ino, ftype in fs.walk():
            if ftype == FT_SYMLINK:
                print(f"{path} -> {fs.readlink(ino)}")
                continue
            size = "" if ftype == FT_DIR else f"  {fs.inode(ino)['size']} bytes"
            print(path + ("/" if ftype == FT_DIR else "") + size)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
