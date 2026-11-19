/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Kernel page tables.
 *
 * vmm_init() replaces the bootloader's tables with the kernel's own:
 *   - the kernel image with real permissions: .text read+execute,
 *     .rodata read-only, .data/.bss read+write, both no-execute;
 *   - the higher-half direct map (HHDM) of all RAM-like memory, no-execute;
 *   - the framebuffer in the HHDM as write-combining;
 *   - nothing at low addresses: a NULL pointer access now faults.
 * The 256 upper-half PML4 entries are allocated up front so that future
 * process address spaces can share the kernel half by copying them.
 */
#include <opaline/arch/cpu.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/string.h>

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)
#define PTE_PCD      (1ULL << 4)
#define PTE_HUGE     (1ULL << 7)
#define PTE_GLOBAL   (1ULL << 8)
#define PTE_NX       (1ULL << 63)
#define PTE_ADDR     0x000FFFFFFFFFF000ULL

#define SIZE_2M (2 * MiB)

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[], __data_start[], __kernel_end[];

static u64 *kernel_pml4;
static u64 kernel_pml4_phys;
static u64 kernel_phys_base;
static bool nx_enabled;

bool vmm_nx_enabled(void) { return nx_enabled; }

static u64 *new_table(void) {
    u64 phys = pmm_alloc_page();
    if (!phys) panic("vmm: out of memory for page tables");
    u64 *t = phys_to_virt(phys);
    memset(t, 0, PAGE_SIZE);
    return t;
}

/* Next-level table for entry i, created if `create`. NULL if absent or if the entry is a huge page. */
static u64 *next_table(u64 *table, unsigned i, bool create, bool user) {
    u64 e = table[i];
    if (e & PTE_PRESENT) {
        if (e & PTE_HUGE) return NULL;
        if (user && !(e & PTE_USER)) table[i] = e | PTE_USER;
        return phys_to_virt(e & PTE_ADDR);
    }
    if (!create) return NULL;
    u64 *t = new_table();
    table[i] = hhdm_to_phys(t) | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
    return t;
}

static u64 pte_bits(u32 flags) {
    u64 b = PTE_PRESENT;
    if (flags & VMM_WRITE) b |= PTE_WRITE;
    if (flags & VMM_USER) b |= PTE_USER;
    if (flags & VMM_GLOBAL) b |= PTE_GLOBAL;
    if (flags & VMM_WC) b |= PTE_PWT;               /* PAT entry 1, programmed as WC below */
    if (flags & VMM_UC) b |= PTE_PCD | PTE_PWT;     /* PAT entry 3: UC */
    if (!(flags & VMM_EXEC) && nx_enabled) b |= PTE_NX;
    return b;
}

static bool map_4k(u64 *pml4, u64 virt, u64 phys, u32 flags) {
    bool user = flags & VMM_USER;
    u64 *pdpt = next_table(pml4, (virt >> 39) & 511, true, user);
    u64 *pd = pdpt ? next_table(pdpt, (virt >> 30) & 511, true, user) : NULL;
    u64 *pt = pd ? next_table(pd, (virt >> 21) & 511, true, user) : NULL;
    if (!pt) return false;   /* already covered by a huge page */
    pt[(virt >> 12) & 511] = phys | pte_bits(flags);
    return true;
}

/* Maps a 2 MiB page if the slot is free; false if a 4 KiB table already sits there. */
static bool map_2m(u64 *pml4, u64 virt, u64 phys, u32 flags) {
    u64 *pdpt = next_table(pml4, (virt >> 39) & 511, true, false);
    u64 *pd = pdpt ? next_table(pdpt, (virt >> 30) & 511, true, false) : NULL;
    if (!pd || (pd[(virt >> 21) & 511] & PTE_PRESENT)) return false;
    pd[(virt >> 21) & 511] = phys | pte_bits(flags) | PTE_HUGE;
    return true;
}

/* Maps [phys, phys+size) at virt, using 2 MiB pages where alignment allows. */
static void map_range(u64 *pml4, u64 virt, u64 phys, u64 size, u32 flags) {
    u64 end = phys + size;
    while (phys < end) {
        if (!(virt % SIZE_2M) && !(phys % SIZE_2M) && end - phys >= SIZE_2M && map_2m(pml4, virt, phys, flags)) {
            virt += SIZE_2M;
            phys += SIZE_2M;
            continue;
        }
        map_4k(pml4, virt, phys, flags);
        virt += PAGE_SIZE;
        phys += PAGE_SIZE;
    }
}

static void map_kernel_section(const char *start, const char *end, u32 flags) {
    u64 vs = ALIGN_DOWN((u64)start, PAGE_SIZE), ve = ALIGN_UP((u64)end, PAGE_SIZE);
    for (u64 v = vs; v < ve; v += PAGE_SIZE)
        map_4k(kernel_pml4, v, kernel_phys_base + (v - KERNEL_BASE), flags | VMM_GLOBAL);
}

static bool in_hhdm(u32 type) {
    switch (type) {
    case BOOT_MEM_USABLE:
    case BOOT_MEM_BOOTLOADER:
    case BOOT_MEM_KERNEL:
    case BOOT_MEM_BOOT_DATA:
    case BOOT_MEM_ACPI_RECLAIMABLE:
    case BOOT_MEM_ACPI_NVS:
    case BOOT_MEM_FIRMWARE_RUNTIME:
        return true;
    default:
        return false;
    }
}

static void setup_cpu_features(void) {
    u32 a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) {
            wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
            nx_enabled = true;
        }
    }
    cpuid(1, 0, &a, &b, &c, &d);
    if (d & (1u << 16)) {   /* PAT: make entry 1 write-combining (default is write-through) */
        u64 pat = rdmsr(MSR_PAT);
        pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8);
        wrmsr(MSR_PAT, pat);
        wbinvd();
    }
    if (d & (1u << 13)) write_cr4(read_cr4() | CR4_PGE);   /* global pages */
}

void vmm_init(const struct boot_info *bi) {
    setup_cpu_features();
    kernel_phys_base = bi->kernel_phys;
    kernel_pml4 = new_table();
    kernel_pml4_phys = hhdm_to_phys(kernel_pml4);
    for (unsigned i = 256; i < 512; i++) next_table(kernel_pml4, i, true, false);

    map_kernel_section(__text_start, __text_end, VMM_EXEC);
    map_kernel_section(__rodata_start, __rodata_end, 0);
    map_kernel_section(__data_start, __kernel_end, VMM_WRITE);

    /* HHDM: merge neighbouring RAM-like regions, then map each run */
    const struct boot_mem_region *r = (const struct boot_mem_region *)bi->mem_regions;
    u64 run_base = 0, run_end = 0;
    for (u64 i = 0; i <= bi->mem_region_count; i++) {
        bool last = i == bi->mem_region_count;
        if (!last && !in_hhdm(r[i].type)) continue;
        u64 b = last ? 0 : ALIGN_DOWN(r[i].base, PAGE_SIZE), e = last ? 0 : ALIGN_UP(r[i].base + r[i].length, PAGE_SIZE);
        if (!last && run_end && b <= run_end) {
            run_end = MAX(run_end, e);
            continue;
        }
        if (run_end) map_range(kernel_pml4, HHDM_BASE + run_base, run_base, run_end - run_base, VMM_WRITE | VMM_GLOBAL);
        run_base = b;
        run_end = e;
    }

    if (bi->fb.base_phys) {
        u64 b = ALIGN_DOWN(bi->fb.base_phys, PAGE_SIZE), e = ALIGN_UP(bi->fb.base_phys + bi->fb.size, PAGE_SIZE);
        map_range(kernel_pml4, HHDM_BASE + b, b, e - b, VMM_WRITE | VMM_GLOBAL | VMM_WC);
    }

    write_cr3(kernel_pml4_phys);
}

static u64 *find_pte(u64 virt) {
    u64 *pdpt = next_table(kernel_pml4, (virt >> 39) & 511, false, false);
    u64 *pd = pdpt ? next_table(pdpt, (virt >> 30) & 511, false, false) : NULL;
    u64 *pt = pd ? next_table(pd, (virt >> 21) & 511, false, false) : NULL;
    return pt ? &pt[(virt >> 12) & 511] : NULL;
}

bool vmm_map(u64 virt, u64 phys, u32 flags) {
    if ((virt | phys) % PAGE_SIZE) panic("vmm_map: unaligned %p -> %p", (void *)virt, (void *)phys);
    bool ok = map_4k(kernel_pml4, virt, phys, flags);
    invlpg(virt);
    return ok;
}

void vmm_unmap(u64 virt) {
    u64 *pte = find_pte(virt);
    if (pte) *pte = 0;
    invlpg(virt);
}

u64 vmm_translate(u64 virt) {
    u64 e = kernel_pml4[(virt >> 39) & 511];
    if (!(e & PTE_PRESENT)) return VMM_NOT_MAPPED;
    u64 *pdpt = phys_to_virt(e & PTE_ADDR);
    e = pdpt[(virt >> 30) & 511];
    if (!(e & PTE_PRESENT)) return VMM_NOT_MAPPED;
    if (e & PTE_HUGE) return (e & PTE_ADDR & ~(GiB - 1)) + (virt & (GiB - 1));
    u64 *pd = phys_to_virt(e & PTE_ADDR);
    e = pd[(virt >> 21) & 511];
    if (!(e & PTE_PRESENT)) return VMM_NOT_MAPPED;
    if (e & PTE_HUGE) return (e & PTE_ADDR & ~(SIZE_2M - 1)) + (virt & (SIZE_2M - 1));
    u64 *pt = phys_to_virt(e & PTE_ADDR);
    e = pt[(virt >> 12) & 511];
    if (!(e & PTE_PRESENT)) return VMM_NOT_MAPPED;
    return (e & PTE_ADDR) + (virt & (PAGE_SIZE - 1));
}

void *vmm_map_mmio(u64 phys, size_t size) {
    u64 b = ALIGN_DOWN(phys, PAGE_SIZE), e = ALIGN_UP(phys + size, PAGE_SIZE);
    for (u64 p = b; p < e; p += PAGE_SIZE) {
        if (vmm_translate(HHDM_BASE + p) != VMM_NOT_MAPPED) continue;   /* already in the HHDM */
        map_4k(kernel_pml4, HHDM_BASE + p, p, VMM_WRITE | VMM_GLOBAL | VMM_UC);
        invlpg(HHDM_BASE + p);
    }
    return phys_to_virt(phys);
}
