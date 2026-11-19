/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Kernel heap.
 *
 * Small allocations (up to 2048 bytes) come from slabs: one 4 KiB page per
 * slab, a 64-byte header, then objects of one size class (16..2048 bytes).
 * Each class keeps a list of slabs that still have free objects. A slab that
 * becomes empty is returned to the page allocator, except the last one of its
 * class, which is kept to avoid churn.
 *
 * Larger allocations take a run of contiguous pages with a 64-byte header.
 *
 * kfree() finds the header from the page the pointer lies in, so it can check
 * the pointer and detect double frees (freed objects carry a marker word).
 */
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/string.h>

#define SLAB_MAGIC   0x534C4142u   /* "SLAB" */
#define LARGE_MAGIC  0x4C415247u   /* "LARG" */
#define HEADER_SIZE  64
#define FREE_MARKER  0xF4EEB10CDEADBEEFULL
#define POISON_BYTE  0xDE

static const u32 class_size[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
#define NUM_CLASSES ARRAY_SIZE(class_size)
#define MAX_SMALL 2048

struct slab {
    u32 magic;
    u32 class_index;
    u32 used, capacity;
    void *free_list;            /* first free object; each free object starts with a next pointer */
    struct slab *next, *prev;   /* list of slabs with free objects */
    u8 reserved[HEADER_SIZE - 40];
};

struct large {
    u32 magic;
    u32 reserved0;
    u64 pages;
    u8 reserved[HEADER_SIZE - 16];
};

_Static_assert(sizeof(struct slab) == HEADER_SIZE, "slab header size");
_Static_assert(sizeof(struct large) == HEADER_SIZE, "large header size");

static struct slab *partial[NUM_CLASSES];
static struct heap_stats stats;

static unsigned class_for(size_t size) {
    unsigned c = 0;
    while (class_size[c] < size) c++;
    return c;
}

static void list_remove(struct slab *s) {
    if (s->prev) s->prev->next = s->next;
    else partial[s->class_index] = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static void list_push(struct slab *s) {
    s->prev = NULL;
    s->next = partial[s->class_index];
    if (s->next) s->next->prev = s;
    partial[s->class_index] = s;
}

static struct slab *new_slab(unsigned c) {
    u64 phys = pmm_alloc_page();
    if (!phys) return NULL;
    struct slab *s = phys_to_virt(phys);
    s->magic = SLAB_MAGIC;
    s->class_index = c;
    s->used = 0;
    s->capacity = (PAGE_SIZE - HEADER_SIZE) / class_size[c];
    s->free_list = NULL;
    u8 *objects = (u8 *)s + HEADER_SIZE;
    for (u32 i = s->capacity; i-- > 0;) {   /* build the free list in address order */
        u64 *obj = (u64 *)(objects + (u64)i * class_size[c]);
        obj[0] = (u64)s->free_list;
        obj[1] = FREE_MARKER;
        s->free_list = obj;
    }
    list_push(s);
    stats.slab_pages++;
    return s;
}

void *kmalloc(size_t size) {
    if (size == 0) size = 1;
    if (size <= MAX_SMALL) {
        unsigned c = class_for(size);
        struct slab *s = partial[c] ? partial[c] : new_slab(c);
        if (!s) return NULL;
        u64 *obj = s->free_list;
        s->free_list = (void *)obj[0];
        obj[0] = obj[1] = 0;
        if (++s->used == s->capacity) list_remove(s);
        stats.live_allocations++;
        stats.bytes_in_use += class_size[c];
        return obj;
    }
    u64 pages = ALIGN_UP(size + HEADER_SIZE, PAGE_SIZE) / PAGE_SIZE;
    u64 phys = pmm_alloc_pages(pages);
    if (!phys) return NULL;
    struct large *l = phys_to_virt(phys);
    l->magic = LARGE_MAGIC;
    l->pages = pages;
    stats.live_allocations++;
    stats.bytes_in_use += pages * PAGE_SIZE - HEADER_SIZE;
    stats.large_pages += pages;
    return (u8 *)l + HEADER_SIZE;
}

void *kcalloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) return NULL;
    void *p = kmalloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

size_t ksize(const void *ptr) {
    const u32 *magic = (const u32 *)ALIGN_DOWN((u64)ptr, PAGE_SIZE);
    if (*magic == SLAB_MAGIC) return class_size[((const struct slab *)magic)->class_index];
    if (*magic == LARGE_MAGIC) return ((const struct large *)magic)->pages * PAGE_SIZE - HEADER_SIZE;
    panic("ksize: %p was not allocated by kmalloc", ptr);
}

void kfree(void *ptr) {
    if (!ptr) return;
    u64 page = ALIGN_DOWN((u64)ptr, PAGE_SIZE);
    if ((u64)ptr < HHDM_BASE || page == (u64)ptr) panic("kfree: invalid pointer %p", ptr);
    u32 magic = *(u32 *)page;

    if (magic == SLAB_MAGIC) {
        struct slab *s = (struct slab *)page;
        u32 size = class_size[s->class_index];
        u64 offset = (u64)ptr - page - HEADER_SIZE;
        if (offset % size || offset / size >= s->capacity) panic("kfree: %p is not the start of an object", ptr);
        u64 *obj = ptr;
        if (obj[1] == FREE_MARKER) panic("kfree: double free of %p", ptr);
        memset(obj, POISON_BYTE, size);
        obj[0] = (u64)s->free_list;
        obj[1] = FREE_MARKER;
        s->free_list = obj;
        if (s->used-- == s->capacity) list_push(s);   /* was full: has room again */
        stats.live_allocations--;
        stats.bytes_in_use -= size;
        if (s->used == 0 && (s->next || s->prev)) {   /* empty, and not the class's only slab */
            list_remove(s);
            s->magic = 0;
            pmm_free_page(hhdm_to_phys(s));
            stats.slab_pages--;
        }
        return;
    }
    if (magic == LARGE_MAGIC) {
        struct large *l = (struct large *)page;
        if ((u64)ptr != page + HEADER_SIZE) panic("kfree: %p is not the start of an allocation", ptr);
        u64 pages = l->pages;
        l->magic = 0;   /* a second kfree of this pointer now fails the magic check */
        stats.live_allocations--;
        stats.bytes_in_use -= pages * PAGE_SIZE - HEADER_SIZE;
        stats.large_pages -= pages;
        pmm_free_pages(hhdm_to_phys(l), pages);
        return;
    }
    panic("kfree: %p was not allocated by kmalloc (or was already freed)", ptr);
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    size_t old = ksize(ptr);
    if (size <= old) return ptr;
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, old);
    kfree(ptr);
    return n;
}

void heap_get_stats(struct heap_stats *out) { *out = stats; }
