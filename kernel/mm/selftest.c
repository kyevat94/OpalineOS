/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Memory self-test, run on every boot. It must leave the allocators exactly
 * as it found them (apart from at most one cached slab per size class).
 */
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/string.h>

#define FAIL(...)                       \
    do {                                \
        console_set_color(COLOR_ERROR); \
        kprintf("  FAILED: " __VA_ARGS__); \
        kprintf("\n");                  \
        return false;                   \
    } while (0)

static u64 rng_state = 0x9E3779B97F4A7C15ULL;

static u64 rng(void) {   /* xorshift64 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static bool test_pmm(void) {
    enum { N = 512 };
    static u64 pages[N];
    struct pmm_stats before, after;
    pmm_get_stats(&before);

    for (int i = 0; i < N; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) FAIL("pmm_alloc_page returned 0 after %d pages", i);
        if (pages[i] % PAGE_SIZE) FAIL("unaligned page %p", (void *)pages[i]);
        u64 *p = phys_to_virt(pages[i]);
        p[0] = pages[i] ^ 0xA5A5A5A5A5A5A5A5ULL;
        p[511] = ~pages[i];
    }
    for (int i = 0; i < N; i++) {
        u64 *p = phys_to_virt(pages[i]);
        if (p[0] != (pages[i] ^ 0xA5A5A5A5A5A5A5A5ULL) || p[511] != ~pages[i])
            FAIL("page %p was overwritten (two allocations share a page?)", (void *)pages[i]);
    }
    for (int i = 0; i < N; i++) pmm_free_page(pages[i]);

    u64 run = pmm_alloc_pages(64);
    if (!run) FAIL("no run of 64 contiguous pages");
    memset(phys_to_virt(run), 0x3C, 64 * PAGE_SIZE);
    pmm_free_pages(run, 64);

    pmm_get_stats(&after);
    if (after.free_pages != before.free_pages)
        FAIL("free pages %lu before, %lu after", before.free_pages, after.free_pages);
    return true;
}

static bool test_vmm(void) {
    const u64 test_virt = 0xFFFFA00000000000ULL;   /* unused part of the kernel half */
    u64 phys = pmm_alloc_page();
    if (!phys) FAIL("no page for the VMM test");
    if (vmm_translate(test_virt) != VMM_NOT_MAPPED) FAIL("test address already mapped");
    if (!vmm_map(test_virt, phys, VMM_WRITE)) FAIL("vmm_map failed");
    if (vmm_translate(test_virt + 123) != phys + 123) FAIL("vmm_translate gives the wrong address");
    *(volatile u64 *)test_virt = 0x0123456789ABCDEFULL;
    if (*(volatile u64 *)phys_to_virt(phys) != 0x0123456789ABCDEFULL) FAIL("write not visible through the HHDM");
    vmm_unmap(test_virt);
    if (vmm_translate(test_virt) != VMM_NOT_MAPPED) FAIL("page still mapped after vmm_unmap");
    pmm_free_page(phys);
    if (vmm_translate(0) != VMM_NOT_MAPPED) FAIL("address 0 is mapped (the identity map should be gone)");
    return true;
}

static u8 pattern(u64 slot, u64 size, u64 i) { return (u8)(slot * 131 + size * 7 + i); }

static bool test_heap(u64 *peak_kib) {
    enum { SLOTS = 2048, OPS = 40000 };
    static u8 *ptr[SLOTS];
    static u32 len[SLOTS];
    struct heap_stats hs;
    struct pmm_stats before, after;
    pmm_get_stats(&before);
    u64 peak = 0;

    for (int op = 0; op < OPS; op++) {
        u64 s = rng() % SLOTS;
        if (!ptr[s]) {
            u64 r = rng() % 100;
            u64 size = r < 90 ? 1 + rng() % 512 : r < 99 ? 513 + rng() % 3584 : 4097 + rng() % 61440;
            ptr[s] = kmalloc(size);
            if (!ptr[s]) FAIL("kmalloc(%lu) returned NULL", size);
            if ((u64)ptr[s] % 16) FAIL("kmalloc(%lu) = %p is not 16-byte aligned", size, ptr[s]);
            if (ksize(ptr[s]) < size) FAIL("ksize %zu < requested %lu", ksize(ptr[s]), size);
            len[s] = (u32)size;
            for (u64 i = 0; i < size; i++) ptr[s][i] = pattern(s, size, i);
        } else {
            for (u64 i = 0; i < len[s]; i++)
                if (ptr[s][i] != pattern(s, len[s], i)) FAIL("block %p (%u bytes) corrupted at byte %lu", ptr[s], len[s], i);
            kfree(ptr[s]);
            ptr[s] = NULL;
        }
        heap_get_stats(&hs);
        peak = MAX(peak, hs.bytes_in_use);
    }
    for (int s = 0; s < SLOTS; s++) {
        kfree(ptr[s]);
        ptr[s] = NULL;
    }

    /* krealloc keeps the contents */
    u8 *p = kmalloc(40);
    for (int i = 0; i < 40; i++) p[i] = (u8)i;
    p = krealloc(p, 5000);
    for (int i = 0; i < 40; i++)
        if (p[i] != (u8)i) FAIL("krealloc lost the contents");
    kfree(p);

    heap_get_stats(&hs);
    if (hs.live_allocations || hs.bytes_in_use || hs.large_pages)
        FAIL("leak: %lu allocations, %lu bytes still in use", hs.live_allocations, hs.bytes_in_use);
    pmm_get_stats(&after);
    if (before.free_pages - after.free_pages > hs.slab_pages)
        FAIL("pages lost: %lu free before, %lu after", before.free_pages, after.free_pages);
    *peak_kib = peak / KiB;
    return true;
}

bool mm_selftest(void) {
    u64 peak = 0;
    if (!test_pmm() || !test_vmm() || !test_heap(&peak)) return false;
    kprintf("Memory self-test passed: pages, mapping and 40000 heap operations (peak %lu KiB)\n", peak);
    return true;
}
