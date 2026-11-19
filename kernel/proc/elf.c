/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Loader for static ELF64 executables (ET_EXEC, x86-64).
 *
 * Every PT_LOAD segment gets fresh zeroed pages in the process address space
 * with the segment's permissions: writable only if PF_W, executable only if
 * PF_X (no-execute otherwise). Bytes past p_filesz stay zero (.bss).
 */
#include <opaline/mm.h>
#include <opaline/proc.h>
#include <opaline/string.h>

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

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

static bool load_segment(u64 page_table, const u8 *file, const Elf64_Phdr *ph, const char **error) {
    u32 flags = VMM_USER | (ph->flags & PF_W ? VMM_WRITE : 0) | (ph->flags & PF_X ? VMM_EXEC : 0);
    u64 seg_start = ph->vaddr, seg_file_end = ph->vaddr + ph->filesz, seg_end = ph->vaddr + ph->memsz;

    for (u64 page = ALIGN_DOWN(seg_start, PAGE_SIZE); page < seg_end; page += PAGE_SIZE) {
        u64 pte, phys = vmm_translate_in(page_table, page, &pte);
        if (phys == VMM_NOT_MAPPED) {
            phys = pmm_alloc_page();
            if (!phys) {
                *error = "out of memory";
                return false;
            }
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
        } else {
            phys = ALIGN_DOWN(phys, PAGE_SIZE);   /* page shared with the previous segment */
            if (pte & 2) flags |= VMM_WRITE;
            if (!(pte & (1ULL << 63))) flags |= VMM_EXEC;
        }
        if (!vmm_map_in(page_table, page, phys, flags)) {
            *error = "could not map a segment";
            return false;
        }
        /* copy the part of the file data that falls into this page */
        u64 from = MAX(page, seg_start), to = MIN(page + PAGE_SIZE, seg_file_end);
        if (from < to) memcpy((u8 *)phys_to_virt(phys) + (from - page), file + ph->offset + (from - seg_start), to - from);
    }
    return true;
}

bool elf_load(u64 page_table, const void *elf, size_t size, u64 *entry, const char **error) {
    const u8 *file = elf;
    const Elf64_Ehdr *eh = elf;
    if (size < sizeof *eh || memcmp(eh->ident, "\x7F" "ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1) {
        *error = "not a 64-bit little-endian ELF file";
        return false;
    }
    if (eh->machine != 0x3E || eh->type != 2) {
        *error = "not an x86-64 executable (ET_EXEC)";
        return false;
    }
    if (eh->phentsize != sizeof(Elf64_Phdr) || eh->phoff + (u64)eh->phnum * sizeof(Elf64_Phdr) > size) {
        *error = "bad program header table";
        return false;
    }
    bool loaded = false;
    for (u16 i = 0; i < eh->phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->phoff) + i;
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > size || ph->offset + ph->filesz < ph->offset) {
            *error = "segment outside the file";
            return false;
        }
        if (ph->vaddr < USER_LOAD_MIN || ph->vaddr + ph->memsz > USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE ||
            ph->vaddr + ph->memsz < ph->vaddr) {
            *error = "segment outside the allowed user range";
            return false;
        }
        if (!load_segment(page_table, file, ph, error)) return false;
        loaded = true;
    }
    if (!loaded) {
        *error = "no loadable segment";
        return false;
    }
    if (eh->entry < USER_LOAD_MIN || eh->entry >= USER_TOP) {
        *error = "entry point outside user space";
        return false;
    }
    *entry = eh->entry;
    return true;
}
