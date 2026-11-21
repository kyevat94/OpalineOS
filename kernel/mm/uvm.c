/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User address spaces: areas (VMAs), demand paging, copy-on-write, shared
 * memory objects, and mmap/munmap/mprotect/mremap/msync/brk. See
 * opaline/uvm.h for the layout and the rules.
 *
 * References: every present page-table entry of a user page holds one
 * reference to the physical page; a memory object holds one more for each of
 * its pages. A private page with more than one reference is shared
 * copy-on-write and is mapped read-only.
 *
 * Locks: mm->lock, then memobj->lock, then the file system's own lock. The
 * kernel never touches user memory while holding them; it reaches pages
 * through the direct map instead.
 */
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/string.h>
#include <opaline/uvm.h>
#include <opaline/vfs.h>

#define PTE_WRITE (1ULL << 1)
#define PTE_USER  (1ULL << 2)
#define PTE_NX    (1ULL << 63)

/* Page-table flags for an area: PROT_NONE pages stay mapped for the kernel but not for the program. */
static u32 vmm_flags(u32 prot) {
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return 0;
    return VMM_USER | (prot & PROT_WRITE ? VMM_WRITE : 0) | (prot & PROT_EXEC ? VMM_EXEC : 0);
}

/* The same flags, back from a page-table entry. */
static u32 pte_flags(u64 pte) {
    return (pte & PTE_USER ? VMM_USER : 0) | (pte & PTE_WRITE ? VMM_WRITE : 0) |
           (!(pte & PTE_NX) || !vmm_nx_enabled() ? VMM_EXEC : 0);
}

/* ---- memory objects (shared mappings) ------------------------------------------------- */

static struct mutex files_lock;   /* the link vnode->memobj */

static struct memobj *memobj_new(struct vnode *file) {
    struct memobj *obj = kcalloc(1, sizeof *obj);
    if (!obj) return NULL;
    obj->refs = 1;
    if (file) {
        vnode_ref(file);
        obj->file = file;
    }
    return obj;
}

/* The page cache of a file, created on its first shared mapping. */
static struct memobj *file_memobj(struct vnode *file) {
    mutex_lock(&files_lock);
    struct memobj *obj = file->memobj;
    if (obj) __atomic_add_fetch(&obj->refs, 1, __ATOMIC_ACQ_REL);
    else if ((obj = memobj_new(file))) file->memobj = obj;
    mutex_unlock(&files_lock);
    return obj;
}

/* The page at `index`, loaded (zero, or read from the file) if needed. obj->lock held. 0: no memory. */
static u64 memobj_page(struct memobj *obj, u64 index) {
    if (index >= obj->count) {
        u64 count = MAX(index + 1, MAX(obj->count * 2, 16));
        u64 *pages = krealloc(obj->pages, count * sizeof *pages);
        if (!pages) return 0;
        memset(pages + obj->count, 0, (count - obj->count) * sizeof *pages);
        obj->pages = pages;
        obj->count = count;
    }
    if (obj->pages[index]) return obj->pages[index];
    u64 phys = pmm_alloc_page();
    if (!phys) return 0;
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    if (obj->file && obj->file->ops->read)
        obj->file->ops->read(obj->file, index * PAGE_SIZE, phys_to_virt(phys), PAGE_SIZE);   /* past the end: zero */
    obj->pages[index] = phys;
    return phys;
}

void memobj_sync(struct memobj *obj) {
    if (!obj->file || !obj->writable || !obj->file->ops->write || !obj->file->ops->stat) return;
    mutex_lock(&obj->lock);
    struct vnode_stat st;
    if (obj->file->ops->stat(obj->file, &st) == 0) {
        /* any page of a writable mapping may have been changed: write them all, never past the end */
        for (u64 i = 0; i < obj->count && i * PAGE_SIZE < st.size; i++)
            if (obj->pages[i])
                obj->file->ops->write(obj->file, i * PAGE_SIZE, phys_to_virt(obj->pages[i]),
                                      MIN(PAGE_SIZE, st.size - i * PAGE_SIZE));
    }
    mutex_unlock(&obj->lock);
}

void memobj_written(struct memobj *obj, u64 offset, const void *data, u64 len) {
    mutex_lock(&obj->lock);
    for (u64 at = offset; at < offset + len;) {
        u64 index = at / PAGE_SIZE, in_page = at % PAGE_SIZE, n = MIN(PAGE_SIZE - in_page, offset + len - at);
        if (index < obj->count && obj->pages[index])
            memcpy((u8 *)phys_to_virt(obj->pages[index]) + in_page, (const u8 *)data + (at - offset), n);
        at += n;
    }
    mutex_unlock(&obj->lock);
}

static void memobj_put(struct memobj *obj) {
    if (obj->file) mutex_lock(&files_lock);   /* nobody may find it while it goes */
    bool last = __atomic_sub_fetch(&obj->refs, 1, __ATOMIC_ACQ_REL) == 0;
    if (last && obj->file) obj->file->memobj = NULL;
    if (obj->file) mutex_unlock(&files_lock);
    if (!last) return;
    memobj_sync(obj);
    for (u64 i = 0; i < obj->count; i++)
        if (obj->pages[i]) pmm_page_put(obj->pages[i]);
    vnode_put(obj->file);
    kfree(obj->pages);
    kfree(obj);
}

/* ---- areas ------------------------------------------------------------------------------ */

static struct vma *find_vma(struct mm *mm, u64 addr) {
    for (struct vma *v = mm->vmas; v && v->start <= addr; v = v->next)
        if (addr < v->end) return v;
    return NULL;
}

static void insert_vma(struct mm *mm, struct vma *n) {
    struct vma **p = &mm->vmas;
    while (*p && (*p)->start < n->start) p = &(*p)->next;
    n->next = *p;
    *p = n;
}

static void unlink_vma(struct mm *mm, struct vma *v) {
    for (struct vma **p = &mm->vmas; *p; p = &(*p)->next)
        if (*p == v) {
            *p = v->next;
            return;
        }
}

static void hold_vma(struct vma *v) {
    if (v->file) vnode_ref(v->file);
    if (v->obj) __atomic_add_fetch(&v->obj->refs, 1, __ATOMIC_ACQ_REL);
}

static void free_vma(struct vma *v) {
    vnode_put(v->file);
    if (v->obj) memobj_put(v->obj);
    kfree(v);
}

/* Splits v at `at` (start < at < end); the second half follows v in the list. */
static bool split_vma(struct vma *v, u64 at) {
    struct vma *n = kmalloc(sizeof *n);
    if (!n) return false;
    *n = *v;
    n->start = at;
    n->offset = v->offset + (at - v->start);
    hold_vma(n);
    v->end = at;
    v->next = n;
    return true;
}

/* Drops the pages of [start, end) from the page table. */
static void release_pages(struct mm *mm, u64 start, u64 end) {
    for (u64 a = start; a < end; a += PAGE_SIZE) {
        u64 phys = vmm_unmap_in(mm->page_table, a);
        if (phys != VMM_NOT_MAPPED) pmm_page_put(phys);
    }
}

/*
 * Makes sure the page at `page` of area v is present (and, for `write`, private to this
 * address space); returns its physical address, 0 if out of memory. mm->lock held.
 */
static u64 ensure_page(struct mm *mm, struct vma *v, u64 page, bool write) {
    u64 pte, phys = vmm_translate_in(mm->page_table, page, &pte);
    if (phys != VMM_NOT_MAPPED) {
        phys = ALIGN_DOWN(phys, PAGE_SIZE);
        if (!write || v->obj) return phys;
        if (pmm_page_refs(phys) > 1) {   /* copy-on-write: this address space gets its own copy */
            u64 copy = pmm_alloc_page();
            if (!copy) return 0;
            memcpy(phys_to_virt(copy), phys_to_virt(phys), PAGE_SIZE);
            if (!vmm_map_in(mm->page_table, page, copy, vmm_flags(v->prot))) {
                pmm_free_page(copy);
                return 0;
            }
            pmm_page_put(phys);
            return copy;
        }
        if (!(pte & PTE_WRITE) && (v->prot & PROT_WRITE))   /* the other copy is gone: writable again */
            vmm_map_in(mm->page_table, page, phys, vmm_flags(v->prot));
        return phys;
    }
    if (v->obj) {
        mutex_lock(&v->obj->lock);
        phys = memobj_page(v->obj, (v->offset + (page - v->start)) / PAGE_SIZE);
        if (phys) pmm_page_ref(phys);   /* the page-table entry's reference */
        mutex_unlock(&v->obj->lock);
        if (!phys) return 0;
    } else {
        phys = pmm_alloc_page();
        if (!phys) return 0;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        if (v->file && v->file->ops->read)   /* a private copy of the file data; past the end: zero */
            v->file->ops->read(v->file, v->offset + (page - v->start), phys_to_virt(phys), PAGE_SIZE);
    }
    if (!vmm_map_in(mm->page_table, page, phys, vmm_flags(v->prot))) {
        pmm_page_put(phys);
        return 0;
    }
    return phys;
}

/* ---- creation and destruction ---------------------------------------------------------- */

struct mm *mm_create(void) {
    struct mm *mm = kcalloc(1, sizeof *mm);
    if (!mm) return NULL;
    mm->page_table = vmm_create_address_space();
    if (!mm->page_table) {
        kfree(mm);
        return NULL;
    }
    mm->refs = 1;
    return mm;
}

void mm_get(struct mm *mm) { __atomic_add_fetch(&mm->refs, 1, __ATOMIC_ACQ_REL); }

void mm_put(struct mm *mm) {
    if (!mm || __atomic_sub_fetch(&mm->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    while (mm->vmas) {
        struct vma *v = mm->vmas;
        mm->vmas = v->next;
        free_vma(v);
    }
    vmm_destroy_address_space(mm->page_table);   /* drops every page reference and frees the tables */
    kfree(mm);
}

struct mm *mm_fork(struct mm *src) {
    struct mm *mm = mm_create();
    if (!mm) return NULL;
    mutex_lock(&src->lock);
    mm->brk_start = src->brk_start;
    mm->brk = src->brk;
    struct vma **tail = &mm->vmas;
    bool ok = true;
    for (struct vma *v = src->vmas; v && ok; v = v->next) {
        struct vma *n = kmalloc(sizeof *n);
        if (!n) {
            ok = false;
            break;
        }
        *n = *v;
        n->next = NULL;
        hold_vma(n);
        *tail = n;
        tail = &n->next;
        if (v->obj) continue;   /* shared: the child finds the same pages when it touches them */
        /* private: both sides keep the pages read-only until one of them writes */
        u32 flags = vmm_flags(v->prot) & ~(u32)VMM_WRITE;
        for (u64 a = v->start; a < v->end; a += PAGE_SIZE) {
            u64 pte, phys = vmm_translate_in(src->page_table, a, &pte);
            if (phys == VMM_NOT_MAPPED) continue;
            phys = ALIGN_DOWN(phys, PAGE_SIZE);
            pmm_page_ref(phys);
            if (!vmm_map_in(mm->page_table, a, phys, flags)) {
                pmm_page_put(phys);
                ok = false;
                break;
            }
            if (pte & PTE_WRITE) vmm_map_in(src->page_table, a, phys, flags);
        }
    }
    mutex_unlock(&src->lock);
    if (!ok) {
        mm_put(mm);
        return NULL;
    }
    return mm;
}

/* ---- mmap and friends ------------------------------------------------------------------- */

static bool range_free(struct mm *mm, u64 start, u64 end) {
    for (struct vma *v = mm->vmas; v && v->start < end; v = v->next)
        if (v->end > start) return false;
    return true;
}

/* Highest free range of `len` bytes below MMAP_TOP. */
static u64 find_free(struct mm *mm, u64 len) {
    u64 best = 0, prev_end = MAX(USER_LOAD_MIN, ALIGN_UP(mm->brk, PAGE_SIZE) + 64 * MiB);
    for (struct vma *v = mm->vmas;; v = v->next) {
        u64 gap_end = v ? MIN(v->start, MMAP_TOP) : MMAP_TOP;
        if (gap_end > prev_end && gap_end - prev_end >= len) best = gap_end - len;
        if (!v || v->start >= MMAP_TOP) break;
        prev_end = MAX(prev_end, v->end);
    }
    return best;
}

/* Removes [start, end) from the area list and drops its pages. Lock held. */
static int unmap_locked(struct mm *mm, u64 start, u64 end) {
    for (struct vma **p = &mm->vmas; *p;) {
        struct vma *v = *p;
        if (v->end <= start) {
            p = &v->next;
            continue;
        }
        if (v->start >= end) break;
        if (v->start < start) {   /* keep the part below */
            if (!split_vma(v, start)) return -ENOMEM;
            p = &v->next;
            continue;
        }
        if (v->end > end && !split_vma(v, end)) return -ENOMEM;
        release_pages(mm, v->start, v->end);
        *p = v->next;
        free_vma(v);
    }
    return 0;
}

i64 mm_map(struct mm *mm, u64 addr, u64 len, u32 prot, u32 flags, struct vnode *file, u64 offset) {
    if (!len || len > USER_TOP) return -EINVAL;
    if (offset % PAGE_SIZE) return -EINVAL;
    u32 type = flags & MAP_SHARED_VALIDATE;
    if (type != MAP_SHARED && type != MAP_PRIVATE && type != MAP_SHARED_VALIDATE) return -EINVAL;
    bool shared = type != MAP_PRIVATE;
    len = ALIGN_UP(len, PAGE_SIZE);
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if (fixed && (addr % PAGE_SIZE || addr < USER_LOAD_MIN || addr + len > USER_STACK_TOP || addr + len < addr))
        return (flags & MAP_FIXED) ? -EINVAL : -ENOMEM;

    struct vma *v = kcalloc(1, sizeof *v);
    if (!v) return -ENOMEM;
    v->prot = prot & (PROT_READ | PROT_WRITE | PROT_EXEC);
    v->kind = VMA_NORMAL;
    v->offset = offset;
    if (shared) {
        v->obj = file ? file_memobj(file) : memobj_new(NULL);
        if (!v->obj) {
            kfree(v);
            return -ENOMEM;
        }
        if (prot & PROT_WRITE) v->obj->writable = true;
        if (!file) v->offset = 0;
    } else if (file) {
        vnode_ref(file);
        v->file = file;
    }

    mutex_lock(&mm->lock);
    int err = 0;
    if (fixed) {
        if (!range_free(mm, addr, addr + len)) {
            if (flags & MAP_FIXED_NOREPLACE) err = -EEXIST;
            else err = unmap_locked(mm, addr, addr + len);
        }
    } else {
        addr = ALIGN_DOWN(addr, PAGE_SIZE);
        if (!addr || addr < USER_LOAD_MIN || addr + len > MMAP_TOP || addr + len < addr || !range_free(mm, addr, addr + len))
            addr = find_free(mm, len);
        if (!addr) err = -ENOMEM;
    }
    if (err) {
        mutex_unlock(&mm->lock);
        free_vma(v);
        return err;
    }
    v->start = addr;
    v->end = addr + len;
    insert_vma(mm, v);
    mutex_unlock(&mm->lock);
    return (i64)addr;
}

int mm_unmap(struct mm *mm, u64 addr, u64 len) {
    if (addr % PAGE_SIZE || !len || addr >= USER_TOP || addr + len > USER_TOP || addr + len < addr) return -EINVAL;
    mutex_lock(&mm->lock);
    int r = unmap_locked(mm, addr, addr + ALIGN_UP(len, PAGE_SIZE));
    mutex_unlock(&mm->lock);
    return r;
}

/* True if every page of [start, end) belongs to some area. Lock held. */
static bool range_mapped(struct mm *mm, u64 start, u64 end) {
    u64 at = start;
    for (struct vma *v = mm->vmas; v && at < end; v = v->next) {
        if (v->end <= at) continue;
        if (v->start > at) break;
        at = v->end;
    }
    return at >= end;
}

int mm_protect(struct mm *mm, u64 addr, u64 len, u32 prot) {
    if (addr % PAGE_SIZE || addr + len < addr || addr + len > USER_TOP) return -EINVAL;
    u64 end = addr + ALIGN_UP(len, PAGE_SIZE);
    prot &= PROT_READ | PROT_WRITE | PROT_EXEC;
    mutex_lock(&mm->lock);
    if (!range_mapped(mm, addr, end)) {
        mutex_unlock(&mm->lock);
        return -ENOMEM;
    }
    int r = 0;
    for (struct vma *v = mm->vmas; v && v->start < end; v = v->next) {
        if (v->end <= addr) continue;
        if (v->start < addr) {
            if (!split_vma(v, addr)) {
                r = -ENOMEM;
                break;
            }
            continue;   /* the next iteration handles the second half */
        }
        if (v->end > end && !split_vma(v, end)) {
            r = -ENOMEM;
            break;
        }
        v->prot = prot;
        if (v->obj && (prot & PROT_WRITE)) v->obj->writable = true;
        for (u64 a = v->start; a < v->end; a += PAGE_SIZE) {
            u64 phys = vmm_translate_in(mm->page_table, a, NULL);
            if (phys == VMM_NOT_MAPPED) continue;
            u32 flags = vmm_flags(prot);
            if (!v->obj && pmm_page_refs(ALIGN_DOWN(phys, PAGE_SIZE)) > 1) flags &= ~(u32)VMM_WRITE;   /* still COW */
            vmm_map_in(mm->page_table, a, ALIGN_DOWN(phys, PAGE_SIZE), flags);
        }
    }
    mutex_unlock(&mm->lock);
    return r;
}

u64 mm_brk(struct mm *mm, u64 addr) {
    mutex_lock(&mm->lock);
    u64 result = mm->brk;
    if (addr >= mm->brk_start && addr < MMAP_TOP) {
        u64 old_end = ALIGN_UP(mm->brk, PAGE_SIZE), new_end = ALIGN_UP(addr, PAGE_SIZE);
        struct vma *heap = NULL;
        for (struct vma *v = mm->vmas; v; v = v->next)
            if (v->kind == VMA_HEAP) heap = v;
        bool ok = true;
        if (new_end > old_end) {
            ok = range_free(mm, old_end, new_end);
            if (ok && heap) {
                heap->end = new_end;
            } else if (ok) {
                heap = kcalloc(1, sizeof *heap);
                ok = heap != NULL;
                if (ok) {
                    heap->start = mm->brk_start;
                    heap->end = new_end;
                    heap->prot = PROT_READ | PROT_WRITE;
                    heap->kind = VMA_HEAP;
                    insert_vma(mm, heap);
                }
            }
        } else if (new_end < old_end && heap) {
            release_pages(mm, new_end, old_end);
            heap->end = new_end;
            if (heap->end == heap->start) {
                unlink_vma(mm, heap);
                kfree(heap);
            }
        }
        if (ok) result = mm->brk = addr;
    }
    mutex_unlock(&mm->lock);
    return result;
}

i64 mm_remap(struct mm *mm, u64 old, u64 old_len, u64 new_len, u32 flags, u64 new_addr) {
    if (old % PAGE_SIZE || !new_len || !old_len || (flags & ~(u32)(MREMAP_MAYMOVE | MREMAP_FIXED))) return -EINVAL;
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE)) return -EINVAL;
    old_len = ALIGN_UP(old_len, PAGE_SIZE);
    new_len = ALIGN_UP(new_len, PAGE_SIZE);
    if (old + old_len < old || old + old_len > USER_TOP || new_len > USER_TOP) return -EINVAL;
    bool fixed = flags & MREMAP_FIXED;
    if (fixed && (new_addr % PAGE_SIZE || new_addr < USER_LOAD_MIN || new_addr + new_len > MMAP_TOP ||
                  (new_addr < old + old_len && new_addr + new_len > old)))
        return -EINVAL;

    mutex_lock(&mm->lock);
    i64 result;
    struct vma *v = find_vma(mm, old);
    if (!v || old + old_len > v->end) {   /* the old range must be one area */
        result = -EFAULT;
        goto out;
    }
    if (!fixed && new_len <= old_len) {   /* shrinking in place */
        result = unmap_locked(mm, old + new_len, old + old_len);
        if (result == 0) result = (i64)old;
        goto out;
    }
    if (!fixed && old + old_len == v->end && old + new_len <= MMAP_TOP && range_free(mm, v->end, old + new_len)) {
        v->end = old + new_len;   /* growing in place */
        if (v->kind == VMA_HEAP) v->kind = VMA_NORMAL;
        result = (i64)old;
        goto out;
    }
    if (!(flags & MREMAP_MAYMOVE)) {
        result = -ENOMEM;
        goto out;
    }
    u64 dest = new_addr;
    if (fixed) {
        result = unmap_locked(mm, dest, dest + new_len);
        if (result < 0) goto out;
    } else if (!(dest = find_free(mm, new_len))) {
        result = -ENOMEM;
        goto out;
    }
    /* the old range becomes an area of its own, which moves */
    v = find_vma(mm, old);
    if (v->start < old) {
        if (!split_vma(v, old)) {
            result = -ENOMEM;
            goto out;
        }
        v = v->next;
    }
    if (v->end > old + old_len && !split_vma(v, old + old_len)) {
        result = -ENOMEM;
        goto out;
    }
    if (new_len < old_len) release_pages(mm, old + new_len, old + old_len);
    unlink_vma(mm, v);
    for (u64 i = 0; i < MIN(old_len, new_len); i += PAGE_SIZE) {   /* the pages move, their references with them */
        u64 pte, phys = vmm_translate_in(mm->page_table, old + i, &pte);
        if (phys == VMM_NOT_MAPPED) continue;
        vmm_unmap_in(mm->page_table, old + i);
        if (!vmm_map_in(mm->page_table, dest + i, ALIGN_DOWN(phys, PAGE_SIZE), pte_flags(pte)))
            pmm_page_put(ALIGN_DOWN(phys, PAGE_SIZE));   /* out of page-table memory: the page is lost, not leaked */
    }
    v->start = dest;
    v->end = dest + new_len;
    if (v->kind == VMA_HEAP) v->kind = VMA_NORMAL;
    insert_vma(mm, v);
    result = (i64)dest;
out:
    mutex_unlock(&mm->lock);
    return result;
}

int mm_sync(struct mm *mm, u64 addr, u64 len) {
    if (addr % PAGE_SIZE || addr + len < addr || addr + len > USER_TOP) return -EINVAL;
    u64 end = addr + ALIGN_UP(len, PAGE_SIZE);
    mutex_lock(&mm->lock);
    int r = range_mapped(mm, addr, end) ? 0 : -ENOMEM;
    for (struct vma *v = mm->vmas; v && v->start < end; v = v->next)
        if (v->end > addr && v->obj) memobj_sync(v->obj);
    mutex_unlock(&mm->lock);
    return r;
}

/* ---- faults and kernel access ----------------------------------------------------------- */

static bool allowed(const struct vma *v, u32 access) {
    if (!(v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return false;
    if ((access & PROT_WRITE) && !(v->prot & PROT_WRITE)) return false;
    if ((access & PROT_EXEC) && !(v->prot & PROT_EXEC)) return false;
    return true;
}

bool mm_fault(struct mm *mm, u64 addr, u32 access) {
    if (addr >= USER_TOP) return false;
    mutex_lock(&mm->lock);
    struct vma *v = find_vma(mm, addr);
    bool ok = v && allowed(v, access);
    if (ok) {
        u64 pte, page = ALIGN_DOWN(addr, PAGE_SIZE);
        u64 phys = vmm_translate_in(mm->page_table, page, &pte);
        if (phys != VMM_NOT_MAPPED && !(pte & PTE_USER))   /* left from an earlier protection */
            vmm_map_in(mm->page_table, page, ALIGN_DOWN(phys, PAGE_SIZE), vmm_flags(v->prot) & ~(u32)VMM_WRITE);
        ok = ensure_page(mm, v, page, access & PROT_WRITE) != 0;   /* also copies a copy-on-write page */
    }
    mutex_unlock(&mm->lock);
    return ok;
}

bool mm_user_access(struct mm *mm, u64 addr, u64 len, bool write) {
    if (addr >= USER_TOP || addr + len > USER_TOP || addr + len < addr) return false;
    if (!len) return true;
    mutex_lock(&mm->lock);
    bool ok = true;
    struct vma *v = NULL;
    for (u64 page = ALIGN_DOWN(addr, PAGE_SIZE); page < addr + len; page += PAGE_SIZE) {
        if (!v || page >= v->end) v = find_vma(mm, page);
        if (!v || !allowed(v, write ? PROT_WRITE : PROT_READ) || !ensure_page(mm, v, page, write)) {
            ok = false;
            break;
        }
    }
    mutex_unlock(&mm->lock);
    return ok;
}

bool mm_write(struct mm *mm, u64 addr, const void *src, u64 len) {
    mutex_lock(&mm->lock);
    bool ok = true;
    while (len) {
        struct vma *v = find_vma(mm, addr);
        u64 phys = v ? ensure_page(mm, v, ALIGN_DOWN(addr, PAGE_SIZE), true) : 0;
        if (!phys) {
            ok = false;
            break;
        }
        u64 n = MIN(len, PAGE_SIZE - addr % PAGE_SIZE);
        memcpy((u8 *)phys_to_virt(phys) + addr % PAGE_SIZE, src, n);
        addr += n;
        src = (const u8 *)src + n;
        len -= n;
    }
    mutex_unlock(&mm->lock);
    return ok;
}

u64 mm_pages(struct mm *mm) {
    u64 n = 0;
    mutex_lock(&mm->lock);
    for (struct vma *v = mm->vmas; v; v = v->next)
        for (u64 a = v->start; a < v->end; a += PAGE_SIZE)
            if (vmm_translate_in(mm->page_table, a, NULL) != VMM_NOT_MAPPED) n++;
    mutex_unlock(&mm->lock);
    return n;
}

void mm_futex_key(struct mm *mm, u64 addr, bool private_op, void **space, u64 *offset) {
    *space = mm;
    *offset = addr;
    if (private_op) return;
    mutex_lock(&mm->lock);
    struct vma *v = find_vma(mm, addr);
    if (v && v->obj) {   /* shared memory: the same key in every process that maps it */
        *space = v->obj;
        *offset = v->offset + (addr - v->start);
    }
    mutex_unlock(&mm->lock);
}
