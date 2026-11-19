/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * ACPI, only what is needed to power off and restart:
 *   RSDP -> XSDT (or RSDT) -> FADT ("FACP") -> PM1a/PM1b control ports, reset register, DSDT;
 *   the \_S5 package in the DSDT gives the "soft off" sleep type values.
 * Powering off = writing SLP_TYP | SLP_EN to the PM1 control port(s).
 *
 * The DSDT is AML bytecode; instead of a full interpreter this finds the
 * "_S5_" name and reads the package that follows, which is how every
 * small operating system does it. Machines that need the full interpreter
 * fall back to the emulator-specific ports, then to halting with a message.
 */
#include <opaline/arch/cpu.h>
#include <opaline/block.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/power.h>
#include <opaline/sched.h>
#include <opaline/string.h>

struct sdt_header {
    char signature[4];
    u32 length;
    u8 revision, checksum;
    char oem_id[6], oem_table_id[8];
    u32 oem_revision, creator_id, creator_revision;
} PACKED;

static struct {
    bool ready;            /* \_S5 found */
    u16 pm1a_cnt, pm1b_cnt;
    u16 slp_typa, slp_typb;
    bool reset_supported;
    u8 reset_space;        /* 1 = I/O port */
    u64 reset_address;
    u8 reset_value;
} acpi;

static const void *map(u64 phys, u64 len) { return vmm_map_mmio(phys, len); }

/* ACPI tables are packed: read fields without assuming alignment */
static u32 rd32(const u8 *p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static u64 rd64(const u8 *p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

static const struct sdt_header *find_table(u64 rsdp_phys, const char *sig) {
    const u8 *rsdp = map(rsdp_phys, 36);
    if (memcmp(rsdp, "RSD PTR ", 8)) return NULL;
    bool xsdt = rsdp[15] >= 2 && rd64(rsdp + 24);
    u64 root_phys = xsdt ? rd64(rsdp + 24) : rd32(rsdp + 16);
    const struct sdt_header *root = map(root_phys, sizeof *root);
    root = map(root_phys, root->length);
    u32 entry_size = xsdt ? 8 : 4;
    u32 count = (root->length - sizeof *root) / entry_size;
    const u8 *entries = (const u8 *)root + sizeof *root;
    for (u32 i = 0; i < count; i++) {
        u64 phys = xsdt ? rd64(entries + i * 8) : rd32(entries + i * 4);
        const struct sdt_header *h = map(phys, sizeof *h);
        if (!memcmp(h->signature, sig, 4)) return map(phys, h->length);
    }
    return NULL;
}

/* Reads one AML integer: ZeroOp, OneOp, BytePrefix n, or a plain byte. Advances *p. */
static u16 aml_small_int(const u8 **p) {
    u8 op = *(*p)++;
    if (op == 0x0A) return *(*p)++;          /* BytePrefix */
    if (op == 0x0B) {                         /* WordPrefix */
        u16 v = (u16)((*p)[0] | (*p)[1] << 8);
        *p += 2;
        return v;
    }
    return op == 0x01 ? 1 : op;               /* OneOp / ZeroOp (0x00) / raw */
}

static void find_s5(const struct sdt_header *dsdt) {
    const u8 *aml = (const u8 *)dsdt + sizeof *dsdt;
    u32 len = dsdt->length - sizeof *dsdt;
    for (u32 i = 0; i + 8 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4)) continue;
        /* NameOp (0x08), optionally followed by the root prefix '\', then "_S5_" */
        if (!(i >= 1 && aml[i - 1] == 0x08) && !(i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == '\\')) continue;
        const u8 *p = aml + i + 4;
        if (*p++ != 0x12) continue;               /* PackageOp */
        p += ((*p >> 6) & 3) + 1;                 /* PkgLength: 1 to 4 bytes */
        p++;                                      /* NumElements */
        acpi.slp_typa = aml_small_int(&p);
        acpi.slp_typb = aml_small_int(&p);
        acpi.ready = true;
        return;
    }
}

void acpi_init(u64 rsdp_phys) {
    if (!rsdp_phys) {
        kprintf("ACPI: no RSDP from the firmware; power off will use fallbacks\n");
        return;
    }
    const u8 *fadt = (const u8 *)find_table(rsdp_phys, "FACP");
    if (!fadt) {
        kprintf("ACPI: no FADT; power off will use fallbacks\n");
        return;
    }
    u32 fadt_len = ((const struct sdt_header *)fadt)->length;
    acpi.pm1a_cnt = (u16) rd32(fadt + 64);
    acpi.pm1b_cnt = (u16) rd32(fadt + 68);
    if (fadt_len >= 129) {
        acpi.reset_supported = (rd32(fadt + 112) >> 10) & 1;   /* RESET_REG_SUP */
        acpi.reset_space = fadt[116];
        acpi.reset_address = rd64(fadt + 120);
        acpi.reset_value = fadt[128];
    }
    u64 dsdt_phys = rd32(fadt + 40);
    if (fadt_len >= 148 && rd64(fadt + 140)) dsdt_phys = rd64(fadt + 140);
    if (dsdt_phys) {
        const struct sdt_header *dsdt = map(dsdt_phys, sizeof(struct sdt_header));
        find_s5(map(dsdt_phys, dsdt->length));
    }
    if (acpi.ready)
        kprintf("ACPI: power off via PM1a control port %#x (S5 sleep type %u)\n", acpi.pm1a_cnt, acpi.slp_typa);
    else
        kprintf("ACPI: \\_S5 not found; power off will use fallbacks\n");
}

static void flush_disks(void) {
    struct block_device *disk = block_first_disk();
    if (disk) block_flush(disk);
}

void system_poweroff(void) {
    console_set_color(COLOR_SECONDARY);
    kprintf("\nShutting down...\n");
    flush_disks();
    cpu_cli();
    if (acpi.ready && acpi.pm1a_cnt) {
        outw(acpi.pm1a_cnt, (u16)(acpi.slp_typa << 10 | 1 << 13));
        if (acpi.pm1b_cnt) outw(acpi.pm1b_cnt, (u16)(acpi.slp_typb << 10 | 1 << 13));
    }
    /* fallbacks: QEMU (q35 and i440fx), Bochs, VirtualBox */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    kprintf("It is now safe to turn off the computer.\n");
    cpu_halt_forever();
}

void system_reboot(void) {
    console_set_color(COLOR_SECONDARY);
    kprintf("\nRestarting...\n");
    flush_disks();
    cpu_cli();
    if (acpi.reset_supported && acpi.reset_space == 1) outb((u16)acpi.reset_address, acpi.reset_value);
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) {}
    outb(0x64, 0xFE);   /* keyboard controller: pulse the reset line */
    /* last resort: a triple fault (no valid IDT, then an exception) */
    struct {
        u16 limit;
        u64 base;
    } PACKED empty = {0, 0};
    __asm__ volatile("lidt %0\n\tint3" : : "m"(empty));
    cpu_halt_forever();
}
