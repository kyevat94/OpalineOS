/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User memory of a process (kernel/mm/uvm.c), stage E6.
 *
 * An address space (struct mm) is a page table plus a sorted list of areas
 * (struct vma): ranges of pages with one protection, either anonymous (zero
 * filled) or backed by a file. Pages are only allocated when first touched:
 * the page-fault handler asks mm_fault() to fill them in (demand paging).
 *
 * Layout of a process (addresses below USER_TOP):
 *   0x10000...              the program (ET_EXEC at its own address; ET_DYN at PIE_BASE)
 *   after the program       the brk heap
 *   ...MMAP_TOP             mmap areas, placed top-down
 *   INTERP_BASE             the dynamic linker (/lib/ld-musl-x86_64.so.1)
 *   ...USER_STACK_TOP       the main thread's stack (USER_STACK_SIZE, demand paged)
 *
 * Simplifications for now: MAP_SHARED mappings behave like MAP_PRIVATE (writes
 * do not reach the file, and fork copies them), and fork copies every page
 * eagerly instead of sharing them copy-on-write.
 */
#ifndef OPALINE_UVM_H
#define OPALINE_UVM_H

#include <opaline/sched.h>
#include <opaline/types.h>

#define USER_STACK_TOP  0x00007FFFFFFFE000ULL
#define USER_STACK_SIZE (8 * MiB)
#define INTERP_BASE     0x00007FF000000000ULL
#define MMAP_TOP        0x00007F0000000000ULL
#define PIE_BASE        0x0000555555554000ULL
#define USER_LOAD_MIN   0x0000000000010000ULL   /* the first 64 KiB are never mapped */

/* mmap() protections and flags (Linux values) */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_GROWSDOWN       0x100
#define MAP_NORESERVE       0x4000
#define MAP_POPULATE        0x8000
#define MAP_STACK           0x20000
#define MAP_FIXED_NOREPLACE 0x100000

struct vnode;

struct vma {
    u64 start, end;           /* page aligned, end exclusive */
    u32 prot;                 /* PROT_* */
    u32 kind;                 /* VMA_* */
    struct vnode *file;       /* NULL: anonymous */
    u64 offset;               /* file offset of `start` */
    struct vma *next;         /* sorted by address */
};

enum { VMA_NORMAL, VMA_HEAP, VMA_STACK };

struct mm {
    u64 page_table;           /* physical address of the PML4 */
    struct vma *vmas;
    u64 brk_start, brk;       /* the heap: [brk_start, brk) */
    int refs;                 /* processes using it (CLONE_VM shares it) */
    struct mutex lock;
};

struct mm *mm_create(void);                 /* empty address space, NULL if out of memory */
struct mm *mm_fork(struct mm *src);         /* a copy of every area and page */
void mm_get(struct mm *mm);
void mm_put(struct mm *mm);                 /* frees it with the last reference (must not be active) */

/* mmap/munmap/mprotect/brk. Return a negative errno on failure. */
i64 mm_map(struct mm *mm, u64 addr, u64 len, u32 prot, u32 flags, struct vnode *file, u64 offset);
int mm_unmap(struct mm *mm, u64 addr, u64 len);
int mm_protect(struct mm *mm, u64 addr, u64 len, u32 prot);
u64 mm_brk(struct mm *mm, u64 addr);

/* Page fault at `addr` (access: PROT_READ, PROT_WRITE or PROT_EXEC). True if the page is now there. */
bool mm_fault(struct mm *mm, u64 addr, u32 access);
/* True if [addr, addr+len) is user memory the process may read (or write); the pages are faulted
   in, so the kernel can then access them directly. */
bool mm_user_access(struct mm *mm, u64 addr, u64 len, bool write);
/* Copies into the address space through the direct map, ignoring protections (program loader). */
bool mm_write(struct mm *mm, u64 addr, const void *src, u64 len);
u64 mm_pages(struct mm *mm);                /* resident pages (for tests) */

#endif
