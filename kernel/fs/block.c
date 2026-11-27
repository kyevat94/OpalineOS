/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Block device registry, write-through block cache and GPT partitions. */
#include <opaline/block.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/string.h>

#define MAX_DEVICES 16
#define CACHE_ENTRIES 256          /* 1 MiB of cached blocks */

static struct block_device *devices[MAX_DEVICES];
static int device_count;

void block_register(struct block_device *dev) {
    if (device_count < MAX_DEVICES) devices[device_count++] = dev;
}

struct block_device *block_find(const char *name) {
    for (int i = 0; i < device_count; i++)
        if (!strcmp(devices[i]->name, name)) return devices[i];
    return NULL;
}

struct block_device *block_first_disk(void) {
    for (int i = 0; i < device_count; i++)
        if (!devices[i]->parent) return devices[i];
    return NULL;
}

/* ---- raw block I/O (partitions add their offset) ----------------------- */

static bool cut_armed, cut_hit;
static u64 cut_allowed, writes_done;

void block_powercut_arm(u64 writes) {
    cut_armed = true;
    cut_hit = false;
    cut_allowed = writes;
    writes_done = 0;
}

u64 block_writes(void) { return writes_done; }
bool block_powercut_hit(void) { return cut_hit; }

static bool raw_io(struct block_device *dev, u64 block, u64 phys, bool write) {
    if (write && cut_armed && ++writes_done > cut_allowed) {
        if (!cut_hit) kprintf("POWERCUT: the disk lost power after %lu writes\n", cut_allowed);
        cut_hit = true;
        return true;   /* the write "succeeds" but never reaches the disk */
    }
    u64 lba = block * SECTORS_PER_BLOCK;
    if (lba + SECTORS_PER_BLOCK > dev->sector_count) return false;   /* past the end of the partition */
    while (dev->parent) {
        lba += dev->start_lba;
        dev = dev->parent;
    }
    if (lba + SECTORS_PER_BLOCK > dev->sector_count) return false;
    return write ? dev->write(dev, lba, SECTORS_PER_BLOCK, phys) : dev->read(dev, lba, SECTORS_PER_BLOCK, phys);
}

/* ---- cache --------------------------------------------------------------- */

struct cache_entry {
    struct block_device *dev;
    u64 block;
    u64 phys;          /* 0 = empty */
    u64 last_use;
};

static struct cache_entry cache[CACHE_ENTRIES];
static u64 use_clock, hits, misses;

static struct cache_entry *lookup(struct block_device *dev, u64 block) {
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (cache[i].phys && cache[i].dev == dev && cache[i].block == block) return &cache[i];
    return NULL;
}

static struct cache_entry *victim(void) {
    struct cache_entry *best = &cache[0];
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].phys) return &cache[i];
        if (cache[i].last_use < best->last_use) best = &cache[i];
    }
    return best;   /* least recently used; write-through, so nothing to save */
}

u8 *block_read(struct block_device *dev, u64 block) {
    struct cache_entry *e = lookup(dev, block);
    if (e) {
        hits++;
        e->last_use = ++use_clock;
        return phys_to_virt(e->phys);
    }
    misses++;
    e = victim();
    if (!e->phys && !(e->phys = pmm_alloc_page())) return NULL;
    e->dev = NULL;   /* invalid until the read succeeds */
    if (!raw_io(dev, block, e->phys, false)) return NULL;
    e->dev = dev;
    e->block = block;
    e->last_use = ++use_clock;
    return phys_to_virt(e->phys);
}

bool block_write(struct block_device *dev, u64 block, const void *data) {
    struct cache_entry *e = lookup(dev, block);
    if (!e) {
        e = victim();
        if (!e->phys && !(e->phys = pmm_alloc_page())) return false;
        e->dev = dev;
        e->block = block;
    }
    if (phys_to_virt(e->phys) != data) memcpy(phys_to_virt(e->phys), data, BLOCK_SIZE);
    e->last_use = ++use_clock;
    if (!raw_io(dev, block, e->phys, true)) {
        e->dev = NULL;   /* the cached copy no longer matches the disk */
        return false;
    }
    return true;
}

bool block_flush(struct block_device *dev) {
    if (cut_hit) return true;
    while (dev->parent) dev = dev->parent;
    return dev->flush ? dev->flush(dev) : true;
}

void block_forget(struct block_device *dev, u64 block) {
    struct cache_entry *e = lookup(dev, block);
    if (e) e->dev = NULL;
}

bool block_write_phys(struct block_device *dev, u64 block, u64 phys) {
    struct cache_entry *e = lookup(dev, block);
    if (e) e->dev = NULL;   /* the cached copy is no longer current */
    return raw_io(dev, block, phys, true);
}

void block_cache_stats(u64 *h, u64 *m) {
    *h = hits;
    *m = misses;
}

/* ---- GPT --------------------------------------------------------------------- */

struct gpt_header {
    char signature[8];
    u32 revision, header_size, header_crc, reserved;
    u64 my_lba, alternate_lba, first_usable, last_usable;
    u8 disk_guid[16];
    u64 entries_lba;
    u32 entry_count, entry_size, entries_crc;
} PACKED;

struct gpt_entry {
    u8 type_guid[16];
    u8 unique_guid[16];
    u64 first_lba, last_lba, attributes;
    u16 name[36];
} PACKED;

static struct block_device partitions[MAX_DEVICES];
static int partition_count;

/* Reads one 512-byte sector through a scratch page (the cache works in 4 KiB blocks). */
static bool read_sector(struct block_device *disk, u64 lba, u8 *out) {
    u64 phys = pmm_alloc_page();
    if (!phys) return false;
    bool ok = disk->read(disk, ALIGN_DOWN(lba, 8), 8, phys);
    if (ok) memcpy(out, (u8 *)phys_to_virt(phys) + (lba % 8) * 512, 512);
    pmm_free_page(phys);
    return ok;
}

struct block_device *gpt_find_partition(struct block_device *disk, const u8 type_guid[16]) {
    static u8 sector[512];
    if (!read_sector(disk, 1, sector)) return NULL;
    struct gpt_header h;
    memcpy(&h, sector, sizeof h);
    if (memcmp(h.signature, "EFI PART", 8) || h.entry_size < sizeof(struct gpt_entry)) return NULL;

    for (u32 i = 0; i < h.entry_count && i < 128; i++) {
        u64 offset = (u64)i * h.entry_size;
        if (!read_sector(disk, h.entries_lba + offset / 512, sector)) return NULL;
        struct gpt_entry e;
        memcpy(&e, sector + offset % 512, sizeof e);
        if (memcmp(e.type_guid, type_guid, 16) || partition_count == MAX_DEVICES) continue;

        struct block_device *p = &partitions[partition_count++];
        ksnprintf(p->name, sizeof p->name, "%sp%u", disk->name, i + 1);
        p->parent = disk;
        p->start_lba = e.first_lba;
        p->sector_count = e.last_lba - e.first_lba + 1;
        block_register(p);
        return p;
    }
    return NULL;
}

u64 block_cache_pages(void) {
    u64 n = 0;
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (cache[i].phys) n++;
    return n;
}
