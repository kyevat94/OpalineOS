/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User address spaces: areas (VMAs), demand paging, mmap/munmap/mprotect/brk
 * and fork copies. See opaline/uvm.h for the layout and the rules.
 *
 * Every change to an mm happens under mm->lock (a sleeping lock: filling a
 * file-backed page reads the disk). The kernel never touches user memory while
 * holding it; it reaches pages through the direct map instead.
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

static void free_vma(struct vma *v) {
    vnode_put(v->file);
    kfree(v);
}

/* Splits v at `at` (start < at < end); the second half follows v in the list. */
static bool split_vma(struct vma *v, u64 at) {
    struct vma *n = kmalloc(sizeof *n);
    if (!n) return false;
    *n = *v;
    n->start = at;
    if (n->file) {
        n->offset = v->offset + (at - v->start);
        vnode_ref(n->file);
    }
    v->end = at;
    v->next = n;
    return true;
}

/* Frees the pages of [start, end). */
static void release_pages(struct mm *mm, u64 start, u64 end) {
    for (u64 a = start; a < end; a += PAGE_SIZE) {
        u64 phys = vmm_unmap_in(mm->page_table, a);
        if (phys != VMM_NOT_MAPPED) pmm_free_page(phys);
    }
}

/* Makes sure the page at `page` of area v is present; returns its physical address (0: no memory). */
static u64 ensure_page(struct mm *mm, struct vma *v, u64 page) {
    u64 phys = vmm_translate_in(mm->page_table, page, NULL);
    if (phys != VMM_NOT_MAPPED) return phys;
    phys = pmm_alloc_page();
    if (!phys) return 0;
    void *mem = phys_to_virt(phys);
    memset(mem, 0, PAGE_SIZE);
    if (v->file && v->file->ops->read) {
        i64 r = v->file->ops->read(v->file, v->offset + (page - v->start), mem, PAGE_SIZE);
        (void)r;   /* past the end of the file the page stays zero */
    }
    if (!vmm_map_in(mm->page_table, page, phys, vmm_flags(v->prot))) {
        pmm_free_page(phys);
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
    vmm_destroy_address_space(mm->page_table);   /* frees every page and page table */
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
        if (n->file) vnode_ref(n->file);
        *tail = n;
        tail = &n->next;
        for (u64 a = v->start; a < v->end; a += PAGE_SIZE) {
            u64 phys = vmm_translate_in(src->page_table, a, NULL);
            if (phys == VMM_NOT_MAPPED) continue;
            u64 copy = pmm_alloc_page();
            if (!copy) {
                ok = false;
                break;
            }
            memcpy(phys_to_virt(copy), phys_to_virt(phys), PAGE_SIZE);
            if (!vmm_map_in(mm->page_table, a, copy, vmm_flags(v->prot))) {
                pmm_free_page(copy);
                ok = false;
                break;
            }
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

/* Removes [start, end) from the area list and frees its pages. Lock held. */
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
    if (!(flags & (MAP_SHARED | MAP_PRIVATE))) return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if (fixed && (addr % PAGE_SIZE || addr < USER_LOAD_MIN || addr + len > USER_STACK_TOP || addr + len < addr))
        return (flags & MAP_FIXED) ? -EINVAL : -ENOMEM;

    struct vma *v = kcalloc(1, sizeof *v);
    if (!v) return -ENOMEM;
    mutex_lock(&mm->lock);
    if (fixed) {
        if (!range_free(mm, addr, addr + len)) {
            if (flags & MAP_FIXED_NOREPLACE) {
                mutex_unlock(&mm->lock);
                kfree(v);
                return -EEXIST;
            }
            int r = unmap_locked(mm, addr, addr + len);
            if (r < 0) {
                mutex_unlock(&mm->lock);
                kfree(v);
                return r;
            }
        }
    } else {
        addr = ALIGN_DOWN(addr, PAGE_SIZE);
        if (!addr || addr < USER_LOAD_MIN || addr + len > MMAP_TOP || addr + len < addr || !range_free(mm, addr, addr + len))
            addr = find_free(mm, len);
        if (!addr) {
            mutex_unlock(&mm->lock);
            kfree(v);
            return -ENOMEM;
        }
    }
    v->start = addr;
    v->end = addr + len;
    v->prot = prot & (PROT_READ | PROT_WRITE | PROT_EXEC);
    v->kind = VMA_NORMAL;
    if (file) {
        vnode_ref(file);
        v->file = file;
        v->offset = offset;
    }
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

int mm_protect(struct mm *mm, u64 addr, u64 len, u32 prot) {
    if (addr % PAGE_SIZE || addr + len < addr || addr + len > USER_TOP) return -EINVAL;
    u64 end = addr + ALIGN_UP(len, PAGE_SIZE);
    prot &= PROT_READ | PROT_WRITE | PROT_EXEC;
    mutex_lock(&mm->lock);
    /* the whole range must be mapped */
    u64 at = addr;
    for (struct vma *v = mm->vmas; v && at < end; v = v->next) {
        if (v->end <= at) continue;
        if (v->start > at) break;
        at = v->end;
    }
    if (at < end) {
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
        for (u64 a = v->start; a < v->end; a += PAGE_SIZE) {
            u64 phys = vmm_translate_in(mm->page_table, a, NULL);
            if (phys != VMM_NOT_MAPPED) vmm_map_in(mm->page_table, a, phys, vmm_flags(prot));
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
                for (struct vma **p = &mm->vmas; *p; p = &(*p)->next)
                    if (*p == heap) {
                        *p = heap->next;
                        break;
                    }
                kfree(heap);
            }
        }
        if (ok) result = mm->brk = addr;
    }
    mutex_unlock(&mm->lock);
    return result;
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
        if (vmm_translate_in(mm->page_table, page, &pte) != VMM_NOT_MAPPED) {
            /* already there (another thread faulted it in): only a real violation fails */
            ok = (pte & PTE_USER) && (!(access & PROT_WRITE) || (pte & PTE_WRITE)) &&
                 (!(access & PROT_EXEC) || !(pte & PTE_NX) || !vmm_nx_enabled());
        } else {
            ok = ensure_page(mm, v, page) != 0;
        }
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
        if (!v || !allowed(v, write ? PROT_WRITE : PROT_READ) || !ensure_page(mm, v, page)) {
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
        u64 phys = v ? ensure_page(mm, v, ALIGN_DOWN(addr, PAGE_SIZE)) : 0;
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
