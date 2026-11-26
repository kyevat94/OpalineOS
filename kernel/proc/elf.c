/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * ELF64 loader (x86-64): fixed-address executables (ET_EXEC) and
 * position-independent ones (ET_DYN: PIE programs and the dynamic linker,
 * loaded at `base`).
 *
 * Every PT_LOAD segment becomes an area of the address space with the
 * segment's permissions: writable only if PF_W, executable only if PF_X. The
 * file data is copied in now; the rest (.bss) stays unmapped until the program
 * touches it, and is then zero. A program that names an interpreter
 * (PT_INTERP) is started through it: the kernel loads both and the
 * interpreter (/lib/ld-musl-x86_64.so.1) links the program.
 */
#include <opaline/mm.h>
#include <opaline/proc.h>
#include <opaline/string.h>
#include <opaline/uvm.h>

typedef struct {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf64_Ehdr;

typedef struct {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} Elf64_Phdr;

#define ET_EXEC 2
#define ET_DYN 3
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4

static u32 prot_of(const Elf64_Phdr *ph) {
    return (ph->flags & PF_R ? PROT_READ : 0) | (ph->flags & PF_W ? PROT_WRITE : 0) | (ph->flags & PF_X ? PROT_EXEC : 0);
}

bool elf_load(struct mm *mm, const void *elf, size_t size, u64 base, struct elf_info *info, const char **error) {
    const u8 *file = elf;
    const Elf64_Ehdr *eh = elf;
    memset(info, 0, sizeof *info);
    if (size < sizeof *eh || memcmp(eh->ident, "\x7F" "ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1) {
        *error = "not a 64-bit little-endian ELF file";
        return false;
    }
    if (eh->machine != 0x3E || (eh->type != ET_EXEC && eh->type != ET_DYN)) {
        *error = "not an x86-64 program";
        return false;
    }
    if (eh->phentsize != sizeof(Elf64_Phdr) || eh->phnum == 0 || eh->phoff > size ||
        (u64)eh->phnum * sizeof(Elf64_Phdr) > size - eh->phoff) {
        *error = "bad program header table";
        return false;
    }
    const Elf64_Phdr *phs = (const Elf64_Phdr *)(file + eh->phoff);

    /* where the image goes: ET_DYN is moved so that its lowest segment starts at `base` */
    u64 lowest = ~0ULL;
    for (u16 i = 0; i < eh->phnum; i++)
        if (phs[i].type == PT_LOAD && phs[i].memsz) lowest = MIN(lowest, ALIGN_DOWN(phs[i].vaddr, PAGE_SIZE));
    if (lowest == ~0ULL) {
        *error = "no loadable segment";
        return false;
    }
    u64 bias = eh->type == ET_DYN ? base - lowest : 0;
    info->base = eh->type == ET_DYN ? base : 0;

    u64 prev_end = 0;
    u32 prev_prot = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const Elf64_Phdr *ph = &phs[i];
        if (ph->type == PT_INTERP) {
            if (ph->filesz < 2 || ph->filesz > sizeof info->interp || ph->offset > size || ph->filesz > size - ph->offset ||
                file[ph->offset + ph->filesz - 1] != 0) {
                *error = "bad interpreter name";
                return false;
            }
            memcpy(info->interp, file + ph->offset, ph->filesz);
            continue;
        }
        if (ph->type == PT_PHDR) {
            info->phdr = ph->vaddr + bias;
            continue;
        }
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->offset > size || ph->filesz > size - ph->offset) {
            *error = "segment outside the file";
            return false;
        }
        u64 start = ph->vaddr + bias, end = start + ph->memsz;
        if (start < USER_LOAD_MIN || end < start || end > USER_STACK_TOP - USER_STACK_SIZE) {
            *error = "segment outside the allowed user range";
            return false;
        }
        u64 lo = ALIGN_DOWN(start, PAGE_SIZE), hi = ALIGN_UP(end, PAGE_SIZE);
        u32 prot = prot_of(ph);
        if (lo < prev_end) {   /* the first page is shared with the previous segment: it gets both rights */
            if (mm_protect(mm, lo, prev_end - lo, prev_prot | prot) < 0) goto nomem;
            lo = prev_end;
        }
        if (lo < hi && mm_map(mm, lo, hi - lo, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0, true) < 0) goto nomem;
        if (ph->filesz && !mm_write(mm, start, file + ph->offset, ph->filesz)) goto nomem;
        if (!info->phdr && eh->phoff >= ph->offset && eh->phoff - ph->offset < ph->filesz)
            info->phdr = start + (eh->phoff - ph->offset);
        prev_end = MAX(prev_end, hi);
        prev_prot = prot;
        info->end = MAX(info->end, end);
    }
    info->entry = eh->entry + bias;
    info->phnum = eh->phnum;
    if (info->entry < USER_LOAD_MIN || info->entry >= USER_TOP) {
        *error = "entry point outside user space";
        return false;
    }
    return true;
nomem:
    *error = "out of memory";
    return false;
}
