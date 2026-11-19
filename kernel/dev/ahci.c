/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * AHCI (SATA) disk driver: QEMU's q35 machine, VirtualBox's "IntelAhci"
 * controller and most PCs since ~2008.
 *
 * Simple and safe first version: one command at a time (slot 0) per port,
 * completion by polling (no interrupts yet), 48-bit LBA DMA commands.
 * Each port gets one page holding its command list (1 KiB), received-FIS
 * area (256 bytes) and a command table with one PRD entry.
 */
#include <opaline/block.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/pci.h>
#include <opaline/sched.h>
#include <opaline/string.h>

/* HBA registers */
#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_PI   0x0C
#define GHC_AE   (1u << 31)
#define GHC_IE   (1u << 1)

/* Port registers (port base = 0x100 + port * 0x80) */
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define TFD_ERR  0x01
#define TFD_DRQ  0x08
#define TFD_BSY  0x80
#define IS_TFES  (1u << 30)
#define SIG_ATA  0x00000101

#define ATA_IDENTIFY     0xEC
#define ATA_READ_DMA_EX  0x25
#define ATA_WRITE_DMA_EX 0x35
#define ATA_FLUSH_EX     0xEA

#define MAX_DISKS 4

struct ahci_port {
    volatile u8 *regs;
    u64 page_phys;          /* command list + FIS + command table */
    struct block_device dev;
};

static struct ahci_port ports[MAX_DISKS];
static int port_count;

static inline u32 rd(volatile u8 *base, u32 reg) { return *(volatile u32 *)(base + reg); }
static inline void wr(volatile u8 *base, u32 reg, u32 v) { *(volatile u32 *)(base + reg) = v; }

/* Spins until (reg & mask) == value or ~1 s passes. */
static bool wait_reg(volatile u8 *base, u32 reg, u32 mask, u32 value) {
    u64 deadline = time_ms() + 1000;
    while ((rd(base, reg) & mask) != value) {
        if (sched_ticks() && time_ms() > deadline) return false;
        __asm__ volatile("pause");
    }
    return true;
}

static bool port_stop(volatile u8 *p) {
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_ST);
    if (!wait_reg(p, PX_CMD, CMD_CR, 0)) return false;
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_FRE);
    return wait_reg(p, PX_CMD, CMD_FR, 0);
}

/*
 * Runs one ATA command on slot 0. `buffer_phys`/`bytes` describe the data
 * (one physical range, bytes <= 4 MiB); `write` sets the direction.
 */
static bool run_command(struct ahci_port *port, u8 command, u64 lba, u16 sectors, u64 buffer_phys, u32 bytes,
                        bool write) {
    volatile u8 *p = port->regs;
    u8 *page = phys_to_virt(port->page_phys);
    u32 *header = (u32 *)page;                    /* command header 0 */
    u8 *table = page + 2048;                      /* command table */
    u64 table_phys = port->page_phys + 2048;

    if (!wait_reg(p, PX_TFD, TFD_BSY | TFD_DRQ, 0)) return false;

    memset(table, 0, 256);
    u8 *fis = table;                              /* register FIS, host to device */
    fis[0] = 0x27;
    fis[1] = 0x80;                                /* this is a command */
    fis[2] = command;
    fis[4] = (u8)lba;
    fis[5] = (u8)(lba >> 8);
    fis[6] = (u8)(lba >> 16);
    fis[7] = 1 << 6;                              /* LBA mode */
    fis[8] = (u8)(lba >> 24);
    fis[9] = (u8)(lba >> 32);
    fis[10] = (u8)(lba >> 40);
    fis[12] = (u8)sectors;
    fis[13] = (u8)(sectors >> 8);

    u32 prd_count = bytes ? 1 : 0;
    if (bytes) {
        u32 *prd = (u32 *)(table + 0x80);
        prd[0] = (u32)buffer_phys;
        prd[1] = (u32)(buffer_phys >> 32);
        prd[2] = 0;
        prd[3] = bytes - 1;                       /* byte count - 1 */
    }
    header[0] = 5 | (write ? 1u << 6 : 0) | (prd_count << 16);   /* 5 dwords of FIS */
    header[1] = 0;                                /* bytes transferred (set by the HBA) */
    header[2] = (u32)table_phys;
    header[3] = (u32)(table_phys >> 32);

    wr(p, PX_IS, ~0u);                            /* clear old status */
    __asm__ volatile("" ::: "memory");
    wr(p, PX_CI, 1);
    u64 deadline = time_ms() + 5000;
    while (rd(p, PX_CI) & 1) {
        if (rd(p, PX_IS) & IS_TFES) break;
        if (time_ms() > deadline) {
            kprintf("ahci: %s: command %#x timed out\n", port->dev.name, command);
            return false;
        }
        __asm__ volatile("pause");
    }
    if ((rd(p, PX_IS) & IS_TFES) || (rd(p, PX_TFD) & TFD_ERR)) {
        kprintf("ahci: %s: command %#x failed at sector %lu (TFD %#x)\n", port->dev.name, command, lba,
                rd(p, PX_TFD));
        return false;
    }
    return true;
}

static bool ahci_read(struct block_device *dev, u64 lba, u32 count, u64 buffer_phys) {
    return run_command(dev->driver, ATA_READ_DMA_EX, lba, (u16)count, buffer_phys, count * 512, false);
}

static bool ahci_write(struct block_device *dev, u64 lba, u32 count, u64 buffer_phys) {
    return run_command(dev->driver, ATA_WRITE_DMA_EX, lba, (u16)count, buffer_phys, count * 512, true);
}

static bool ahci_flush(struct block_device *dev) {
    return run_command(dev->driver, ATA_FLUSH_EX, 0, 0, 0, 0, false);
}

static bool port_init(volatile u8 *p, struct ahci_port *port) {
    if (!port_stop(p)) return false;
    port->regs = p;
    port->page_phys = pmm_alloc_page();
    if (!port->page_phys) return false;
    memset(phys_to_virt(port->page_phys), 0, PAGE_SIZE);
    wr(p, PX_CLB, (u32)port->page_phys);
    wr(p, PX_CLBU, (u32)(port->page_phys >> 32));
    wr(p, PX_FB, (u32)(port->page_phys + 1024));
    wr(p, PX_FBU, (u32)((port->page_phys + 1024) >> 32));
    wr(p, PX_SERR, ~0u);
    wr(p, PX_IS, ~0u);
    wr(p, PX_IE, 0);                              /* polling */
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE);
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_ST);
    return true;
}

int ahci_init(void) {
    struct pci_device pci;
    int found = 0;
    for (int ctrl = 0; pci_find_class(0x01, 0x06, 0x01, ctrl, &pci); ctrl++) {
        pci_enable_dma(&pci);
        u64 abar = pci_bar_address(&pci, 5);
        if (!abar) continue;
        volatile u8 *hba = vmm_map_mmio(abar, 0x1100);
        wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_AE);
        wr(hba, HBA_GHC, rd(hba, HBA_GHC) & ~GHC_IE);
        u32 implemented = rd(hba, HBA_PI);

        for (int i = 0; i < 32 && port_count < MAX_DISKS; i++) {
            if (!(implemented & (1u << i))) continue;
            volatile u8 *p = hba + 0x100 + i * 0x80;
            u32 ssts = rd(p, PX_SSTS);
            if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;   /* no device, or not active */
            if (rd(p, PX_SIG) != SIG_ATA) continue;                          /* ATAPI etc. */

            struct ahci_port *port = &ports[port_count];
            if (!port_init(p, port)) continue;
            ksnprintf(port->dev.name, sizeof port->dev.name, "sata%d", port_count);
            port->dev.driver = port;

            /* IDENTIFY DEVICE: size and model */
            u64 id_phys = pmm_alloc_page();
            if (!id_phys || !run_command(port, ATA_IDENTIFY, 0, 0, id_phys, 512, false)) {
                if (id_phys) pmm_free_page(id_phys);
                continue;
            }
            const u16 *id = phys_to_virt(id_phys);
            u64 sectors = (u64)id[100] | (u64)id[101] << 16 | (u64)id[102] << 32 | (u64)id[103] << 48;
            if (!sectors) sectors = (u64)id[60] | (u64)id[61] << 16;   /* no 48-bit addressing */
            char model[41];
            for (int w = 0; w < 20; w++) {   /* ATA strings swap the bytes of each word */
                model[w * 2] = (char)(id[27 + w] >> 8);
                model[w * 2 + 1] = (char)id[27 + w];
            }
            model[40] = 0;
            for (int k = 39; k >= 0 && model[k] == ' '; k--) model[k] = 0;
            pmm_free_page(id_phys);

            port->dev.sector_count = sectors;
            port->dev.read = ahci_read;
            port->dev.write = ahci_write;
            port->dev.flush = ahci_flush;
            block_register(&port->dev);
            kprintf("Disk %s: %s, %lu MiB (AHCI port %d)\n", port->dev.name, model, sectors * 512 / MiB, i);
            port_count++;
            found++;
        }
    }
    return found;
}
