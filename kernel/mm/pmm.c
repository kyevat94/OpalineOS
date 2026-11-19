/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Physical page allocator: one bit per 4 KiB page (1 = used).
 *
 * Managed memory: the "usable" regions of the boot memory map plus the
 * "bootloader" regions (the bootloader's code and buffers are dead once the
 * kernel runs; everything the kernel keeps is in "kernel"/"boot data").
 * The first 1 MiB is never handed out (kept for SMP startup code later).
 */
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>

#define LOW_MEMORY_LIMIT (1 * MiB)

static spinlock_t lock = SPINLOCK_INIT;

static u64 *bitmap;          /* HHDM pointer */
static u64 page_count;       /* pages covered by the bitmap */
static u64 total_pages, free_pages;
static u64 search_hint;      /* word index where the next search starts */

static inline bool bit_used(u64 page) { return bitmap[page / 64] & (1ULL << (page % 64)); }
static inline void bit_set(u64 page) { bitmap[page / 64] |= 1ULL << (page % 64); }
static inline void bit_clear(u64 page) { bitmap[page / 64] &= ~(1ULL << (page % 64)); }

static bool managed(u32 type) { return type == BOOT_MEM_USABLE || type == BOOT_MEM_BOOTLOADER; }

void pmm_init(const struct boot_info *bi) {
    const struct boot_mem_region *r = (const struct boot_mem_region *)bi->mem_regions;
    u64 n = bi->mem_region_count;

    u64 top = 0;
    for (u64 i = 0; i < n; i++)
        if (managed(r[i].type)) top = MAX(top, r[i].base + r[i].length);
    page_count = top / PAGE_SIZE;
    u64 bitmap_bytes = ALIGN_UP((page_count + 63) / 64 * 8, PAGE_SIZE);

    /* the bitmap goes into the first usable region (above 1 MiB) that can hold it */
    u64 bitmap_phys = 0;
    for (u64 i = 0; i < n && !bitmap_phys; i++) {
        if (r[i].type != BOOT_MEM_USABLE) continue;
        u64 base = MAX(ALIGN_UP(r[i].base, PAGE_SIZE), LOW_MEMORY_LIMIT);
        u64 end = ALIGN_DOWN(r[i].base + r[i].length, PAGE_SIZE);
        if (end > base && end - base >= bitmap_bytes) bitmap_phys = base;
    }
    if (!bitmap_phys) panic("pmm: no room for the page bitmap (%lu KiB)", bitmap_bytes / KiB);

    bitmap = phys_to_virt(bitmap_phys);
    memset(bitmap, 0xFF, bitmap_bytes);   /* everything used until proven free */

    for (u64 i = 0; i < n; i++) {
        if (!managed(r[i].type)) continue;
        u64 first = ALIGN_UP(r[i].base, PAGE_SIZE) / PAGE_SIZE;
        u64 last = ALIGN_DOWN(r[i].base + r[i].length, PAGE_SIZE) / PAGE_SIZE;
        for (u64 p = first; p < last; p++) {
            if (p * PAGE_SIZE < LOW_MEMORY_LIMIT) continue;
            if (p * PAGE_SIZE >= bitmap_phys && p * PAGE_SIZE < bitmap_phys + bitmap_bytes) continue;
            if (bit_used(p)) {
                bit_clear(p);
                free_pages++;
            }
        }
    }
    total_pages = free_pages;
}

static u64 alloc_one(void) {
    u64 words = (page_count + 63) / 64;
    for (u64 k = 0; k < words; k++) {
        u64 w = (search_hint + k) % words;
        if (bitmap[w] == ~0ULL) continue;
        u64 page = w * 64 + (u64)__builtin_ctzll(~bitmap[w]);
        if (page >= page_count) continue;
        bit_set(page);
        free_pages--;
        search_hint = w;
        return page * PAGE_SIZE;
    }
    return 0;
}

static u64 alloc_run(size_t count) {
    u64 run = 0;
    for (u64 p = 0; p < page_count; p++) {
        if (bit_used(p)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            u64 first = p + 1 - count;
            for (u64 q = first; q <= p; q++) bit_set(q);
            free_pages -= count;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

u64 pmm_alloc_page(void) {
    u64 flags = spin_lock(&lock);
    u64 phys = alloc_one();
    spin_unlock(&lock, flags);
    return phys;
}

u64 pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    u64 flags = spin_lock(&lock);
    u64 phys = count == 1 ? alloc_one() : alloc_run(count);
    spin_unlock(&lock, flags);
    return phys;
}

void pmm_free_pages(u64 phys, size_t count) {
    if (phys % PAGE_SIZE) panic("pmm: freeing unaligned address %p", (void *)phys);
    u64 flags = spin_lock(&lock);
    for (u64 p = phys / PAGE_SIZE; p < phys / PAGE_SIZE + count; p++) {
        if (p >= page_count) panic("pmm: freeing page %p outside managed memory", (void *)(p * PAGE_SIZE));
        if (!bit_used(p)) panic("pmm: double free of page %p", (void *)(p * PAGE_SIZE));
        bit_clear(p);
        free_pages++;
    }
    spin_unlock(&lock, flags);
}

void pmm_free_page(u64 phys) { pmm_free_pages(phys, 1); }

void pmm_get_stats(struct pmm_stats *out) {
    u64 flags = spin_lock(&lock);
    out->total_pages = total_pages;
    out->free_pages = free_pages;
    spin_unlock(&lock, flags);
}
