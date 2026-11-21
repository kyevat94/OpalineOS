/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Memory management.
 *
 *   pmm   physical page allocator (bitmap of 4 KiB pages)       kernel/mm/pmm.c
 *   vmm   kernel page tables, mapping and protection             kernel/mm/vmm.c
 *   heap  kmalloc/kfree (slabs for small sizes, pages for large) kernel/mm/heap.c
 *
 * All physical memory the kernel uses is reachable through the higher-half
 * direct map (HHDM): virtual = physical + HHDM_BASE.
 *
 * Not thread-safe yet: there is a single CPU and no preemption until stage E3,
 * which adds locking.
 */
#ifndef OPALINE_MM_H
#define OPALINE_MM_H

#include <boot/bootinfo.h>
#include <opaline/types.h>

#define PAGE_SIZE   ((u64)4096)
#define PAGE_SHIFT  12
#define HHDM_BASE   BOOT_HHDM_BASE
#define KERNEL_BASE BOOT_KERNEL_BASE

static inline void *phys_to_virt(u64 phys) { return (void *)(phys + HHDM_BASE); }
static inline u64 hhdm_to_phys(const void *virt) { return (u64)virt - HHDM_BASE; }

/* ---- physical pages --------------------------------------------------- */

void pmm_init(const struct boot_info *bi);
u64 pmm_alloc_page(void);              /* physical address, 0 if out of memory (not zeroed) */
u64 pmm_alloc_pages(size_t count);     /* physically contiguous run */
void pmm_free_page(u64 phys);
void pmm_free_pages(u64 phys, size_t count);
/* Reference counts (user pages that may be shared): a new page has 1; put frees it at 0. */
void pmm_page_ref(u64 phys);
void pmm_page_put(u64 phys);
u32 pmm_page_refs(u64 phys);

struct pmm_stats {
    u64 total_pages;    /* pages the allocator manages */
    u64 free_pages;
};
void pmm_get_stats(struct pmm_stats *out);

/* ---- virtual memory --------------------------------------------------- */

enum {
    VMM_WRITE  = 1 << 0,
    VMM_EXEC   = 1 << 1,
    VMM_USER   = 1 << 2,
    VMM_GLOBAL = 1 << 3,
    VMM_WC     = 1 << 4,   /* write-combining (framebuffers) */
    VMM_UC     = 1 << 5,   /* uncached (device registers) */
};

#define VMM_NOT_MAPPED (~0ULL)

/* Builds the kernel's own page tables and switches to them (the identity map is dropped). */
void vmm_init(const struct boot_info *bi);
bool vmm_map(u64 virt, u64 phys, u32 flags);      /* one 4 KiB page in the kernel address space */
void vmm_unmap(u64 virt);
u64 vmm_translate(u64 virt);                       /* physical address, or VMM_NOT_MAPPED */
void *vmm_map_mmio(u64 phys, size_t size);         /* uncached, inside the HHDM */
u64 vmm_alloc_stack(size_t pages);                 /* kernel stack with a guard page; returns its top, 0 if no memory */
void vmm_free_stack(u64 top, size_t pages);

/* ---- process address spaces ---------------------------------------------
 * A process address space is a PML4 (physical address) whose upper half is
 * shared with the kernel and whose lower half (below USER_TOP) is private. */

#define USER_TOP 0x0000800000000000ULL   /* first address that is not user space */

u64 vmm_kernel_page_table(void);
u64 vmm_create_address_space(void);                        /* 0 if out of memory */
bool vmm_map_in(u64 page_table, u64 virt, u64 phys, u32 flags);
u64 vmm_translate_in(u64 page_table, u64 virt, u64 *pte);  /* pte: raw entry, may be NULL */
u64 vmm_unmap_in(u64 page_table, u64 virt);                /* the page's physical address, or VMM_NOT_MAPPED */
/* Frees every user page and page table of the address space, then the PML4. */
void vmm_destroy_address_space(u64 page_table);
bool vmm_nx_enabled(void);

/* ---- kernel heap ------------------------------------------------------ */

void *kmalloc(size_t size);            /* 16-byte aligned, NULL if out of memory */
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);                 /* panics on double free or a foreign pointer */
size_t ksize(const void *ptr);         /* usable size of an allocation */

struct heap_stats {
    u64 live_allocations;
    u64 bytes_in_use;       /* usable bytes of all live allocations */
    u64 slab_pages;
    u64 large_pages;
};
void heap_get_stats(struct heap_stats *out);

/* Memory self-test run at boot (kernel/mm/selftest.c). */
bool mm_selftest(void);

#endif
