/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User memory of a process (kernel/mm/uvm.c).
 *
 * An address space (struct mm) is a page table plus a sorted list of areas
 * (struct vma): ranges of pages with one protection. Pages are only allocated
 * when first touched: the page-fault handler asks mm_fault() to fill them in
 * (demand paging). Physical pages are reference counted (pmm_page_ref/put).
 *
 * Kinds of areas:
 *   private anonymous   zero-filled pages of this process (heap, stack, malloc)
 *   private file        a copy of the file's data, written changes stay private
 *   shared              the pages of a memory object (struct memobj): a file's page
 *                       cache (MAP_SHARED of a file) or shared anonymous memory;
 *                       every process mapping it sees the same pages
 *
 * fork shares private pages copy-on-write: both processes map them read-only
 * and the first write gets its own copy. Shared areas stay shared.
 *
 * Layout of a process (addresses below USER_TOP):
 *   0x10000...              the program (ET_EXEC at its own address; ET_DYN at PIE_BASE)
 *   after the program       the brk heap
 *   ...MMAP_TOP             mmap areas, placed top-down
 *   INTERP_BASE             the dynamic linker (/lib/ld-musl-x86_64.so.1)
 *   ...USER_STACK_TOP       the main thread's stack (USER_STACK_SIZE, demand paged)
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
#define MAP_SHARED_VALIDATE 0x03
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_GROWSDOWN       0x100
#define MAP_NORESERVE       0x4000
#define MAP_POPULATE        0x8000
#define MAP_STACK           0x20000
#define MAP_FIXED_NOREPLACE 0x100000
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

struct vnode;

/*
 * Pages shared by every mapping of one object. For a file it is the file's page
 * cache (vnode->memobj while any shared mapping exists); read() and write() of
 * the file stay consistent with it. Pages are found by page index.
 */
struct memobj {
    int refs;                 /* shared areas using it */
    struct vnode *file;       /* NULL: shared anonymous memory */
    bool writable;            /* some mapping may write: written back on sync */
    u64 *pages;               /* physical address per page index, 0 = not loaded */
    u64 count;                /* entries in pages[] */
    struct mutex lock;
};

struct vma {
    u64 start, end;           /* page aligned, end exclusive */
    u32 prot;                 /* PROT_* */
    u32 kind;                 /* VMA_* */
    struct vnode *file;       /* private file mapping (NULL: anonymous) */
    struct memobj *obj;       /* shared mapping (NULL: private) */
    u64 offset;               /* file / object offset of `start` */
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
struct mm *mm_fork(struct mm *src);         /* private pages copy-on-write, shared areas shared */
void mm_get(struct mm *mm);
void mm_put(struct mm *mm);                 /* frees it with the last reference (must not be active) */

/* mmap/munmap/mprotect/brk/mremap/msync. Return a negative errno on failure. */
i64 mm_map(struct mm *mm, u64 addr, u64 len, u32 prot, u32 flags, struct vnode *file, u64 offset);
int mm_unmap(struct mm *mm, u64 addr, u64 len);
int mm_protect(struct mm *mm, u64 addr, u64 len, u32 prot);
u64 mm_brk(struct mm *mm, u64 addr);
i64 mm_remap(struct mm *mm, u64 old, u64 old_len, u64 new_len, u32 flags, u64 new_addr);
int mm_sync(struct mm *mm, u64 addr, u64 len);

/* Page fault at `addr` (access: PROT_READ, PROT_WRITE or PROT_EXEC). True if the page is now there. */
bool mm_fault(struct mm *mm, u64 addr, u32 access);
/* True if [addr, addr+len) is user memory the process may read (or write); the pages are faulted
   in (and made private for writing), so the kernel can then access them directly. */
bool mm_user_access(struct mm *mm, u64 addr, u64 len, bool write);
/* Copies into the address space through the direct map, ignoring protections (program loader,
   clone's CLONE_CHILD_SETTID). Copy-on-write pages get their own copy first. */
bool mm_write(struct mm *mm, u64 addr, const void *src, u64 len);
u64 mm_pages(struct mm *mm);                /* resident pages (for tests) */

/* Futex key of a user address: the memory object and offset for shared areas, else (mm, addr). */
void mm_futex_key(struct mm *mm, u64 addr, bool private_op, void **space, u64 *offset);

/* The page cache side of shared file mappings (used by the VFS). */
void memobj_sync(struct memobj *obj);       /* writes the pages back to the file */
void memobj_written(struct memobj *obj, u64 offset, const void *data, u64 len);   /* write() went to the file */

#endif
