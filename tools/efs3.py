# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""EFS3, the OpalineOS file system: creates images from a folder and reads them back.

    python tools/efs3.py ls   DISK.img [PATH]     # list a directory of the root partition
    python tools/efs3.py cat  DISK.img PATH       # print a file
    python tools/efs3.py tree DISK.img            # whole tree with sizes
    python tools/efs3.py check DISK.img           # consistency check (bitmaps, link counts)

Other tools import make_fs() (build) and Efs3 (read).

On-disk format (little endian, 4 KiB blocks; kernel/fs/efs3.c is the other implementation):

    block 0              superblock
    block 1              inode bitmap (bit n = inode n in use; inode 0 is never used)
    block 2..            block bitmap (bit n = block n in use, metadata included)
    then                 inode table (128-byte inodes, inode n at byte n * 128)
    then                 data blocks

    superblock:  "EFS3", version, block_size, block_count, inode_count, inode_bitmap_block,
                 block_bitmap_block, block_bitmap_blocks, inode_table_block, inode_table_blocks,
                 first_data_block, root_inode (1), free_blocks, free_inodes, reserved,
                 label[32] (UTF-8), uuid[16]
    inode:       mode u16 (0x4000 dir / 0x8000 file | permissions), links u16, flags u32,
                 size u64, ctime u64, mtime u64, direct[12] u32, indirect u32,
                 double_indirect u32, 40 reserved bytes  (block number 0 = hole/none)
    directory:   records that cover every byte of each data block:
                 inode u32 (0 = free), rec_len u16 (multiple of 4), name_len u8,
                 type u8 (1 file, 2 directory), name (UTF-8, NFC, no '/' or NUL, 1..255 bytes)

Names are stored NFC-normalized, so "ação" typed with combining accents is the same file.
"""
import os
import struct
import sys
import unicodedata
import uuid

BLOCK = 4096
INODE_SIZE = 128
PTRS = BLOCK // 4                    # block numbers per indirect block
MAGIC = b"EFS3"
ROOT_INODE = 1
S_IFDIR, S_IFREG = 0x4000, 0x8000
FT_FILE, FT_DIR = 1, 2

SUPER_FMT = "<4s14I32s16s"
INODE_FMT = "<HHIQQQ12III40s"
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
    return inode_count, bbmap_blocks, itable, itable_blocks, itable + itable_blocks


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


def make_fs(source_dir, size_bytes, label="OpalineOS"):
    """Builds an EFS3 image of `size_bytes` containing the files under `source_dir`."""
    block_count = size_bytes // BLOCK
    inode_count, bbmap_blocks, itable, itable_blocks, first_data = layout(block_count)
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
                inodes[child] = dict(mode=S_IFREG | (0o755 if executable else 0o644), links=1,
                                     size=len(data), direct=d, ind=i, dind=di)
                children.append((raw, child, FT_FILE, None))
        entries = [(b".", ino, FT_DIR), (b"..", parent, FT_DIR)] + [(r, c, t) for r, c, t, _ in children]
        data = b"".join(dir_records(entries))
        d, i, di = write_data(data)
        subdirs = sum(1 for c in children if c[2] == FT_DIR)
        inodes[ino] = dict(mode=S_IFDIR | 0o755, links=2 + subdirs, size=len(data), direct=d, ind=i, dind=di)
        for raw, child, ftype, full in children:
            if ftype == FT_DIR:
                add_dir(full, child, ino)

    root = alloc_inode()
    assert root == ROOT_INODE
    add_dir(source_dir, root, root)

    for n, ino in inodes.items():
        struct.pack_into(INODE_FMT, img, itable * BLOCK + n * INODE_SIZE, ino["mode"], ino["links"], 0,
                         ino["size"], 0, 0, *ino["direct"], ino["ind"], ino["dind"], b"\0" * 40)
    used_inodes = next_inode[0]          # 0 .. used_inodes-1 are taken (0 is reserved)
    for n in range(used_inodes):
        img[BLOCK + n // 8] |= 1 << (n % 8)
    for b in range(next_block[0]):       # metadata + data are all below next_block
        img[2 * BLOCK + b // 8] |= 1 << (b % 8)
    for b in range(block_count, bbmap_blocks * BLOCK * 8):   # bits past the end: never free
        img[2 * BLOCK + b // 8] |= 1 << (b % 8)

    vol_uuid = uuid.uuid5(ROOT_TYPE_GUID, label).bytes
    struct.pack_into(SUPER_FMT, img, 0, MAGIC, 1, BLOCK, block_count, inode_count, 1, 2, bbmap_blocks, itable,
                     itable_blocks, first_data, ROOT_INODE, block_count - next_block[0],
                     inode_count - used_inodes, 0, label.encode("utf-8")[:32].ljust(32, b"\0"), vol_uuid)
    return bytes(img)


class Efs3:
    """Reads an EFS3 file system (bytes of the partition)."""

    def __init__(self, data):
        self.data = data
        f = struct.unpack_from(SUPER_FMT, data, 0)
        if f[0] != MAGIC:
            raise ValueError("not an EFS3 file system")
        (_, self.version, bs, self.block_count, self.inode_count, _, _, _, self.itable, _, _, self.root,
         self.free_blocks, self.free_inodes, _, label, _) = f
        self.label = label.rstrip(b"\0").decode("utf-8")

    @classmethod
    def from_disk(cls, path):
        """Opens the root partition of a GPT disk image."""
        with open(path, "rb") as f:
            disk = f.read()
        hdr = disk[512:512 + 92]
        entries_lba, count, size = struct.unpack_from("<QII", hdr, 72)
        for i in range(count):
            e = disk[entries_lba * 512 + i * size:entries_lba * 512 + (i + 1) * size]
            if uuid.UUID(bytes_le=e[:16]) == ROOT_TYPE_GUID:
                first, last = struct.unpack_from("<QQ", e, 32)
                return cls(disk[first * 512:(last + 1) * 512])
        raise ValueError("no OpalineOS root partition in this disk image")

    def inode(self, n):
        v = struct.unpack_from(INODE_FMT, self.data, self.itable * BLOCK + n * INODE_SIZE)
        return dict(mode=v[0], links=v[1], size=v[3], direct=list(v[6:18]), ind=v[18], dind=v[19])

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
        """fsck: compares the bitmaps and free counts with what the directory tree really uses."""
        errors = []
        _, bbmap_blocks, itable, itable_blocks, first_data = layout(self.block_count)
        used_blocks = set(range(first_data))
        used_inodes = {0}
        links = {}

        def mark_block(b, owner):
            if b in used_blocks:
                errors.append(f"block {b} used twice ({owner})")
            if not first_data <= b < self.block_count:
                errors.append(f"block {b} out of range ({owner})")
            used_blocks.add(b)

        def visit(n, path, parent):
            if n in used_inodes:
                errors.append(f"inode {n} reached twice ({path})")
                return
            used_inodes.add(n)
            ino = self.inode(n)
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
            if ino["mode"] & 0xF000 == S_IFDIR:
                entries = self.listdir(n)
                names = [e[0] for e in entries]
                if names[:2] != [".", ".."]:
                    errors.append(f"{path}: '.' and '..' missing")
                elif entries[1][1] != parent:
                    errors.append(f"{path}: '..' points to inode {entries[1][1]}, expected {parent}")
                subdirs = 0
                for name, child, ftype in entries[2:]:
                    links[child] = links.get(child, 0) + 1
                    if ftype == FT_DIR:
                        subdirs += 1
                        visit(child, path.rstrip("/") + "/" + name, n)
                    else:
                        visit(child, path.rstrip("/") + "/" + name, n)
                if ino["links"] != 2 + subdirs:
                    errors.append(f"{path}: link count {ino['links']}, expected {2 + subdirs}")
            elif ino["links"] != links.get(n, 0):
                errors.append(f"{path}: link count {ino['links']}, expected {links.get(n, 0)}")

        visit(self.root, "/", self.root)
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
    fs = Efs3.from_disk(image)
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
        for path, ino, ftype in fs.walk():
            size = "" if ftype == FT_DIR else f"  {fs.inode(ino)['size']} bytes"
            print(path + ("/" if ftype == FT_DIR else "") + size)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
