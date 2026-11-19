# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""Builds the OpalineOS disk image: a GPT disk with an EFI System Partition (FAT32).

Written from scratch (no mtools, no admin rights) so the build works on any
machine with Python 3.

    python tools/mkimage.py OUT.img --file EFI/BOOT/BOOTX64.EFI=build/BOOTX64.EFI [--file ...]
    python tools/mkimage.py OUT.img --verify     # re-reads an image and checks GPT + FAT32

Layout (512-byte sectors):
    LBA 0             protective MBR
    LBA 1             GPT header
    LBA 2..33         GPT partition entries (128 x 128 bytes)
    LBA 2048..        EFI System Partition, FAT32
    ...               free space (future OpalineOS root partition)
    last 33 sectors   backup GPT entries + backup header
"""
import argparse
import datetime
import struct
import sys
import uuid
import zlib

SECTOR = 512
DISK_MB = 128
ESP_START = 2048                    # 1 MiB alignment
ESP_MB = 64
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_SECTORS = GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR   # 32

ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
NS = uuid.UUID("6f70616c-696e-654f-5300-000000000000")      # namespace for reproducible GUIDs
DISK_GUID = uuid.uuid5(NS, "disk")
ESP_GUID = uuid.uuid5(NS, "esp")

# FAT32 parameters
RESERVED_SECTORS = 32
NUM_FATS = 2
SECTORS_PER_CLUSTER = 1
CLUSTER = SECTOR * SECTORS_PER_CLUSTER
EOC = 0x0FFFFFFF
VOLUME_LABEL = b"OPALINE ESP"     # exactly 11 bytes
VOLUME_ID = 0x0A11E05            # fixed, so builds are reproducible


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


# --------------------------------------------------------------------------
# FAT32
# --------------------------------------------------------------------------

def short_name(name):
    """'BOOTX64.EFI' -> b'BOOTX64 EFI'. Only plain uppercase 8.3 names are allowed."""
    base, _, ext = name.partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
    if not (1 <= len(base) <= 8 and len(ext) <= 3 and set(base + ext) <= ok):
        raise ValueError(f"not a valid uppercase 8.3 name: {name!r}")
    return base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def fat_datetime(now):
    date = ((now.year - 1980) << 9) | (now.month << 5) | now.day
    time = (now.hour << 11) | (now.minute << 5) | (now.second // 2)
    return date, time


def dir_entry(name11, attr, cluster, size, now):
    date, time = fat_datetime(now)
    return struct.pack("<11sBBBHHHHHHHI", name11, attr, 0, 0, time, date, date,
                       cluster >> 16, time, date, cluster & 0xFFFF, size)


class Fat32Builder:
    def __init__(self, total_sectors, hidden_sectors):
        self.total = total_sectors
        self.hidden = hidden_sectors
        # FAT size: iterate until the cluster count and the FAT size agree.
        fat_sectors = 1
        while True:
            data_sectors = self.total - RESERVED_SECTORS - NUM_FATS * fat_sectors
            clusters = data_sectors // SECTORS_PER_CLUSTER
            needed = -(-(clusters + 2) * 4 // SECTOR)
            if needed <= fat_sectors:
                break
            fat_sectors = needed
        if clusters < 65525:
            raise ValueError(f"partition too small for FAT32 ({clusters} clusters)")
        self.fat_sectors = fat_sectors
        self.clusters = clusters
        self.data_start = RESERVED_SECTORS + NUM_FATS * fat_sectors
        self.fat = [0] * (clusters + 2)
        self.fat[0] = 0x0FFFFF00 | 0xF8
        self.fat[1] = EOC
        self.next_free = 2
        self.data = {}                 # cluster -> bytes
        self.now = datetime.datetime.now()
        # root directory: dict name -> node ; node = ("dir", children) | ("file", bytes)
        self.tree = {}

    def add_file(self, path, content):
        parts = path.strip("/").split("/")
        node = self.tree
        for part in parts[:-1]:
            child = node.setdefault(part.upper(), ("dir", {}))
            if child[0] != "dir":
                raise ValueError(f"{part} is a file, not a directory")
            node = child[1]
        node[parts[-1].upper()] = ("file", content)

    def alloc_chain(self, nbytes):
        count = max(1, -(-nbytes // CLUSTER))
        first = self.next_free
        if first + count - 1 > self.clusters + 1:
            raise ValueError("partition full")
        for i in range(count):
            c = first + i
            self.fat[c] = EOC if i == count - 1 else c + 1
        self.next_free += count
        return first

    def write_chain(self, first, content):
        c = first
        for off in range(0, max(len(content), 1), CLUSTER):
            self.data[c] = content[off:off + CLUSTER]
            nxt = self.fat[c]
            c = nxt if nxt != EOC else None

    def build_dir(self, children, own_cluster, parent_cluster, is_root):
        """Allocates clusters for the children, writes them, and returns this directory's bytes."""
        entries = []
        if is_root:
            entries.append(dir_entry(VOLUME_LABEL, 0x08, 0, 0, self.now))
        else:
            entries.append(dir_entry(b".          ", 0x10, own_cluster, 0, self.now))
            entries.append(dir_entry(b"..         ", 0x10, parent_cluster, 0, self.now))
        for name, (kind, payload) in sorted(children.items()):
            if kind == "file":
                first = self.alloc_chain(len(payload))
                self.write_chain(first, payload)
                entries.append(dir_entry(short_name(name), 0x20, first, len(payload), self.now))
            else:
                n_entries = 2 + len(payload)
                first = self.alloc_chain(n_entries * 32)
                # ".." of a directory directly under root points to cluster 0
                content = self.build_dir(payload, first, 0 if is_root else own_cluster, False)
                self.write_chain(first, content)
                entries.append(dir_entry(short_name(name), 0x10, first, 0, self.now))
        return b"".join(entries)

    def build(self):
        root_entries = 1 + len(self.tree)
        root = self.alloc_chain(root_entries * 32)
        assert root == 2
        self.write_chain(root, self.build_dir(self.tree, root, 0, True))

        out = bytearray(self.total * SECTOR)
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x58\x90"
        boot[3:11] = b"OPALINE "
        struct.pack_into("<HBHBHHBHHHII", boot, 11,
                         SECTOR, SECTORS_PER_CLUSTER, RESERVED_SECTORS, NUM_FATS,
                         0, 0, 0xF8, 0, 63, 255, self.hidden, self.total)
        struct.pack_into("<IHHIHH12sBBB", boot, 36,
                         self.fat_sectors, 0, 0, 2, 1, 6, b"\0" * 12, 0x80, 0, 0x29)
        struct.pack_into("<I11s8s", boot, 67, VOLUME_ID, VOLUME_LABEL, b"FAT32   ")
        boot[510:512] = b"\x55\xAA"

        free = self.clusters - (self.next_free - 2)
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<III", fsinfo, 484, 0x61417272, free, self.next_free)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)

        out[0:SECTOR] = boot
        out[SECTOR:2 * SECTOR] = fsinfo
        out[6 * SECTOR:7 * SECTOR] = boot          # backup boot sector
        out[7 * SECTOR:8 * SECTOR] = fsinfo        # backup FSInfo

        fat_bytes = struct.pack(f"<{len(self.fat)}I", *self.fat)
        for i in range(NUM_FATS):
            off = (RESERVED_SECTORS + i * self.fat_sectors) * SECTOR
            out[off:off + len(fat_bytes)] = fat_bytes

        for c, chunk in self.data.items():
            off = (self.data_start + (c - 2) * SECTORS_PER_CLUSTER) * SECTOR
            out[off:off + len(chunk)] = chunk
        return bytes(out)


# --------------------------------------------------------------------------
# GPT
# --------------------------------------------------------------------------

def gpt_entry(type_guid, unique_guid, first, last, name):
    return struct.pack("<16s16sQQQ72s", type_guid.bytes_le, unique_guid.bytes_le,
                       first, last, 0, name.encode("utf-16-le").ljust(72, b"\0"))


def gpt_header(my_lba, alt_lba, first_usable, last_usable, entries_lba, entries_crc, total):
    def pack(crc):
        return struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, crc, 0,
                           my_lba, alt_lba, first_usable, last_usable, DISK_GUID.bytes_le,
                           entries_lba, GPT_ENTRIES, GPT_ENTRY_SIZE, entries_crc)
    hdr = pack(0)
    return pack(crc32(hdr)).ljust(SECTOR, b"\0")


def build_image(files):
    total = DISK_MB * 1024 * 1024 // SECTOR
    esp_sectors = ESP_MB * 1024 * 1024 // SECTOR
    esp_last = ESP_START + esp_sectors - 1
    first_usable = 2 + GPT_ENTRY_SECTORS
    last_usable = total - 2 - GPT_ENTRY_SECTORS

    fat = Fat32Builder(esp_sectors, ESP_START)
    for path, content in files:
        fat.add_file(path, content)
    esp = fat.build()

    entries = gpt_entry(ESP_TYPE, ESP_GUID, ESP_START, esp_last, "EFI System Partition")
    entries = entries.ljust(GPT_ENTRIES * GPT_ENTRY_SIZE, b"\0")
    entries_crc = crc32(entries)

    img = bytearray(total * SECTOR)
    mbr = bytearray(SECTOR)
    # protective MBR: one partition of type 0xEE covering the whole disk
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\x00\x02\x00", 0xEE, b"\xFF\xFF\xFF",
                     1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr

    img[SECTOR:2 * SECTOR] = gpt_header(1, total - 1, first_usable, last_usable, 2, entries_crc, total)
    img[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    backup_entries_lba = total - 1 - GPT_ENTRY_SECTORS
    img[backup_entries_lba * SECTOR:(backup_entries_lba * SECTOR) + len(entries)] = entries
    img[(total - 1) * SECTOR:] = gpt_header(total - 1, 1, first_usable, last_usable,
                                           backup_entries_lba, entries_crc, total)

    img[ESP_START * SECTOR:ESP_START * SECTOR + len(esp)] = esp
    return bytes(img)


# --------------------------------------------------------------------------
# Verification (reads the image back the way a firmware would)
# --------------------------------------------------------------------------

def verify_image(img, expected):
    errors = []
    total = len(img) // SECTOR
    if img[510:512] != b"\x55\xAA" or img[450] != 0xEE:
        errors.append("protective MBR missing")

    def check_header(lba):
        hdr = img[lba * SECTOR:lba * SECTOR + 92]
        (sig, rev, size, crc, _, my, alt, fu, lu, dguid, elba, n, esz, ecrc) = \
            struct.unpack("<8sIIIIQQQQ16sQIII", hdr)
        zeroed = hdr[:16] + b"\0\0\0\0" + hdr[20:]
        if sig != b"EFI PART" or crc32(zeroed) != crc or my != lba:
            errors.append(f"GPT header at LBA {lba} invalid")
        ents = img[elba * SECTOR:elba * SECTOR + n * esz]
        if crc32(ents) != ecrc:
            errors.append(f"GPT entries CRC wrong (header at LBA {lba})")
        return ents

    ents = check_header(1)
    check_header(total - 1)
    tguid, _, first, last = struct.unpack("<16s16sQQ", ents[:48])
    if uuid.UUID(bytes_le=tguid) != ESP_TYPE:
        errors.append("first partition is not an EFI System Partition")

    base = first * SECTOR
    bs = img[base:base + SECTOR]
    bps, spc, rsvd, nfats = struct.unpack("<HBHB", bs[11:17])
    fatsz, root_clus = struct.unpack("<I", bs[36:40])[0], struct.unpack("<I", bs[44:48])[0]
    if bs[82:90] != b"FAT32   " or bps != SECTOR:
        errors.append("ESP is not FAT32")
    fat_off = base + rsvd * bps
    data_off = base + (rsvd + nfats * fatsz) * bps
    csize = spc * bps

    def next_cluster(c):
        return struct.unpack("<I", img[fat_off + 4 * c:fat_off + 4 * c + 4])[0] & 0x0FFFFFFF

    def read_chain(c, size=None):
        out = bytearray()
        while c < 0x0FFFFFF8:
            out += img[data_off + (c - 2) * csize:data_off + (c - 1) * csize]
            c = next_cluster(c)
        return bytes(out[:size]) if size is not None else bytes(out)

    def find(path):
        cluster = root_clus
        parts = path.upper().split("/")
        for i, part in enumerate(parts):
            raw = read_chain(cluster)
            name11 = short_name(part)
            for off in range(0, len(raw), 32):
                e = raw[off:off + 32]
                if e[0] == 0:
                    break
                if e[:11] == name11:
                    clus = (struct.unpack("<H", e[20:22])[0] << 16) | struct.unpack("<H", e[26:28])[0]
                    if i == len(parts) - 1:
                        return read_chain(clus, struct.unpack("<I", e[28:32])[0])
                    cluster = clus
                    break
            else:
                return None
        return None

    for path, content in expected:
        got = find(path)
        if got is None:
            errors.append(f"{path} not found on the ESP")
        elif got != content:
            errors.append(f"{path} content differs")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--file", action="append", default=[], metavar="ESP_PATH=LOCAL_FILE")
    ap.add_argument("--verify", action="store_true", help="only check an existing image")
    args = ap.parse_args()

    files = []
    for spec in args.file:
        dest, _, src = spec.partition("=")
        with open(src, "rb") as f:
            files.append((dest, f.read()))

    if args.verify:
        with open(args.image, "rb") as f:
            img = f.read()
    else:
        img = build_image(files)
        with open(args.image, "wb") as f:
            f.write(img)

    errors = verify_image(img, files)
    if errors:
        for e in errors:
            print("ERROR:", e, file=sys.stderr)
        sys.exit(1)
    print(f"OK: {args.image} ({len(img) // (1024 * 1024)} MiB, GPT + FAT32 ESP, {len(files)} file(s))")


if __name__ == "__main__":
    main()
