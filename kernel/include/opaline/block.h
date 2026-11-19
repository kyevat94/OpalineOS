/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Block devices, the block cache and partitions.
 *
 * File systems work in 4 KiB blocks. The cache keeps recently used blocks in
 * physical pages (which the disk driver can use for DMA directly) and is
 * write-through: block_write() reaches the disk before it returns.
 * Callers must serialize access (the VFS holds one lock for all file systems).
 */
#ifndef OPALINE_BLOCK_H
#define OPALINE_BLOCK_H

#include <opaline/types.h>

#define BLOCK_SIZE 4096
#define SECTORS_PER_BLOCK (BLOCK_SIZE / 512)

struct block_device {
    char name[32];
    u64 sector_count;         /* 512-byte sectors */
    /* sector I/O into one physical page (count <= 8, buffer page-aligned) */
    bool (*read)(struct block_device *dev, u64 lba, u32 count, u64 buffer_phys);
    bool (*write)(struct block_device *dev, u64 lba, u32 count, u64 buffer_phys);
    bool (*flush)(struct block_device *dev);
    void *driver;
    struct block_device *parent;   /* partitions: the whole disk */
    u64 start_lba;                 /* partitions: first sector on the parent */
};

void block_register(struct block_device *dev);
struct block_device *block_find(const char *name);
struct block_device *block_first_disk(void);

/* Cached access by 4 KiB block number. Returns a pointer valid until the next cache call; NULL on I/O error. */
u8 *block_read(struct block_device *dev, u64 block);
/* Writes `data` (BLOCK_SIZE bytes) to the block, through the cache, to the disk. */
bool block_write(struct block_device *dev, u64 block, const void *data);
bool block_flush(struct block_device *dev);
void block_cache_stats(u64 *hits, u64 *misses);
u64 block_cache_pages(void);   /* pages the cache holds (they are not leaked memory) */

/* Reads the GPT of `disk` and registers a partition device for the first partition with `type_guid`. */
struct block_device *gpt_find_partition(struct block_device *disk, const u8 type_guid[16]);

/* Disk drivers */
int ahci_init(void);   /* returns the number of disks registered */

#endif
