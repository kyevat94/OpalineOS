/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/gdt.h>
#include <opaline/string.h>

struct tss {
    u32 reserved0;
    u64 rsp[3];          /* stacks used when entering ring 0 from rings 0-2 */
    u64 reserved1;
    u64 ist[7];          /* interrupt stack table (IST1..IST7) */
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} PACKED;

struct gdt_pointer {
    u16 limit;
    u64 base;
} PACKED;

#define IST_STACK_SIZE (16 * 1024)

static u64 gdt[7] ALIGNED(16);
static struct tss tss ALIGNED(16);
static u8 ist_stacks[3][IST_STACK_SIZE] ALIGNED(16);

void gdt_load(const struct gdt_pointer *gdtr);
void tss_load(u16 selector);

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* kernel code: 64-bit, DPL 0 */
    gdt[2] = 0x00CF92000000FFFFULL;   /* kernel data: DPL 0 */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* user data: DPL 3 */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* user code: 64-bit, DPL 3 */

    memset(&tss, 0, sizeof tss);
    for (int i = 0; i < 3; i++) tss.ist[i] = (u64)&ist_stacks[i][IST_STACK_SIZE];
    tss.iomap_base = sizeof tss;      /* no I/O permission bitmap */

    /* the TSS descriptor takes two GDT slots in long mode */
    u64 base = (u64)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |   /* present, 64-bit TSS */
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdt_pointer gdtr = {sizeof gdt - 1, (u64)gdt};
    gdt_load(&gdtr);
    tss_load(GDT_TSS);
}
