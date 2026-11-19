/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * OpalineOS UEFI bootloader.
 *
 * 1. Reads \OPALINE\KERNEL.ELF, \OPALINE\CONSOLE.FNT and the optional
 *    \OPALINE\CMDLINE.TXT from the partition it was loaded from.
 * 2. Loads the kernel's ELF segments into fresh physical pages.
 * 3. Picks a graphics mode (GOP) and records the framebuffer.
 * 4. Builds page tables (identity map + higher-half direct map + kernel).
 * 5. Exits boot services, converts the memory map and jumps to the kernel
 *    with a struct boot_info (see boot/bootinfo.h).
 *
 * Progress and errors go to the screen and to COM1.
 */
#include "efi.h"
#include "../bootinfo.h"

#define BOOTLOADER_VERSION "0.1.0"
#define PAGE_SIZE 4096ULL
#define MAX_REGIONS 512

/* ---- small libc ------------------------------------------------------ */

void *memset(void *dst, int value, UINTN n) {
    UINT8 *d = dst;
    while (n--) *d++ = (UINT8)value;
    return dst;
}

void *memcpy(void *dst, const void *src, UINTN n) {
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

/* ---- output: COM1 + UEFI console -------------------------------------- */

#define COM1 0x3F8

static inline void outb(UINT16 port, UINT8 value) { __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port)); }
static inline UINT8 inb(UINT16 port) {
    UINT8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x03);
}

static void serial_putc(char c) {
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++) {}
    outb(COM1, (UINT8)c);
}

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static int console_alive = 1;    /* the UEFI console is gone after ExitBootServices */

/* Writes an ASCII string to COM1 and (while boot services exist) to the screen. */
static void print(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\n') serial_putc('\r');
        serial_putc(*p);
    }
    if (!console_alive) return;
    CHAR16 buf[160];
    UINTN n = 0;
    for (; *s; s++) {
        if (*s == '\n') buf[n++] = u'\r';
        buf[n++] = (CHAR16)(UINT8)*s;
        if (n >= 156) {
            buf[n] = 0;
            ST->ConOut->OutputString(ST->ConOut, buf);
            n = 0;
        }
    }
    buf[n] = 0;
    ST->ConOut->OutputString(ST->ConOut, buf);
}

static void print_hex(UINT64 v) {
    char out[19] = "0x";
    for (int i = 0; i < 16; i++) out[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    out[18] = 0;
    print(out);
}

static void print_dec(UINT64 v) {
    char tmp[21];
    int i = 20;
    tmp[i] = 0;
    do {
        tmp[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    print(tmp + i);
}

/* Stops with a message; waits for a key and shuts down. Never returns. */
static void fail(const char *what, EFI_STATUS status) {
    if (console_alive) ST->ConOut->SetAttribute(ST->ConOut, EFI_LIGHTRED | EFI_BACKGROUND_BLACK);
    print("\n[boot] ERROR: ");
    print(what);
    print(" (status ");
    print_hex(status);
    print(")\n");
    if (console_alive) {
        print("Press any key to shut down.\n");
        EFI_INPUT_KEY key;
        ST->ConIn->Reset(ST->ConIn, 0);
        while (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_NOT_READY) BS->Stall(10000);
        ST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, 0);
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---- allocation ------------------------------------------------------ */

/*
 * Everything is allocated as EfiLoaderData. (The UEFI specification lets OS
 * loaders use memory types >= 0x80000000, but VirtualBox's firmware crashes in
 * ExitBootServices when they are used.) Allocations the kernel must keep are
 * remembered here and carved out of the memory map with their own type.
 */
#define MAX_OWNED 8
static struct { UINT64 base, length; UINT32 type; } owned[MAX_OWNED];
static int owned_count;

/* `tag` is BOOT_MEM_KERNEL or BOOT_MEM_BOOT_DATA for memory the kernel keeps, 0 for temporary buffers. */
static UINT64 alloc_pages(UINTN pages, UINT32 tag, const char *what) {
    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS s = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(s)) fail(what, s);
    memset((void *)addr, 0, pages * PAGE_SIZE);
    if (tag) {
        if (owned_count == MAX_OWNED) fail("too many boot allocations", 0);
        owned[owned_count].base = addr;
        owned[owned_count].length = pages * PAGE_SIZE;
        owned[owned_count].type = tag;
        owned_count++;
    }
    return addr;
}

/* ---- files ----------------------------------------------------------- */

static EFI_FILE_PROTOCOL *root;

/* Reads a whole file into new pages (`tag` as in alloc_pages). Returns 0 if it does not exist. */
static UINT64 read_file(const CHAR16 *path, UINT32 tag, UINT64 *size_out) {
    EFI_FILE_PROTOCOL *f;
    if (EFI_ERROR(root->Open(root, &f, path, EFI_FILE_MODE_READ, 0))) return 0;
    UINT64 size = 0;
    f->SetPosition(f, ~0ULL);          /* all ones = end of file */
    f->GetPosition(f, &size);
    f->SetPosition(f, 0);
    UINT64 buf = alloc_pages((size + PAGE_SIZE) / PAGE_SIZE, tag, "out of memory reading a file");
    UINTN n = size;
    EFI_STATUS s = f->Read(f, &n, (void *)buf);
    f->Close(f);
    if (EFI_ERROR(s) || n != size) fail("could not read a file", s);
    *size_out = size;
    return buf;
}

/* ---- ELF loading ----------------------------------------------------- */

typedef struct {
    UINT8  ident[16];
    UINT16 type, machine;
    UINT32 version;
    UINT64 entry, phoff, shoff;
    UINT32 flags;
    UINT16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 type, flags;
    UINT64 offset, vaddr, paddr, filesz, memsz, align;
} Elf64_Phdr;

#define PT_LOAD 1

static UINT64 kernel_phys, kernel_vbase, kernel_pages, kernel_entry;

static void load_kernel(UINT64 file, UINT64 size) {
    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (size < sizeof *eh || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 || eh->machine != 0x3E || eh->type != 2)
        fail("KERNEL.ELF is not an x86-64 ELF executable", 0);

    UINT64 vmin = ~0ULL, vmax = 0;
    for (UINT16 i = 0; i < eh->phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->phoff + (UINT64)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->vaddr < vmin) vmin = ph->vaddr;
        if (ph->vaddr + ph->memsz > vmax) vmax = ph->vaddr + ph->memsz;
    }
    if (vmin == ~0ULL || vmin < BOOT_KERNEL_BASE) fail("kernel is not linked at the expected address", 0);

    kernel_vbase = vmin & ~(PAGE_SIZE - 1);
    kernel_pages = (vmax - kernel_vbase + PAGE_SIZE - 1) / PAGE_SIZE;
    kernel_phys = alloc_pages(kernel_pages, BOOT_MEM_KERNEL, "no memory for the kernel");

    for (UINT16 i = 0; i < eh->phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->phoff + (UINT64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->offset + ph->filesz > size) fail("KERNEL.ELF is truncated", 0);
        memcpy((void *)(kernel_phys + (ph->vaddr - kernel_vbase)), (void *)(file + ph->offset), ph->filesz);
        /* the rest (memsz - filesz, i.e. .bss) is already zero */
    }
    kernel_entry = eh->entry;
}

/* ---- graphics -------------------------------------------------------- */

static int shift_of(UINT32 mask) {
    int s = 0;
    while (mask && !(mask & 1)) { mask >>= 1; s++; }
    return s;
}

/* Parses "video=WxH" from the command line. */
static void parse_video(const char *cmd, UINT32 *w, UINT32 *h) {
    for (const char *p = cmd; *p; p++) {
        const char *k = "video=";
        int i = 0;
        while (k[i] && p[i] == k[i]) i++;
        if (k[i]) continue;
        const char *q = p + i;
        UINT32 a = 0, b = 0;
        while (*q >= '0' && *q <= '9') a = a * 10 + (UINT32)(*q++ - '0');
        if (*q++ != 'x') return;
        while (*q >= '0' && *q <= '9') b = b * 10 + (UINT32)(*q++ - '0');
        *w = a;
        *h = b;
        return;
    }
}

static void setup_graphics(struct boot_info *bi) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (EFI_ERROR(BS->LocateProtocol(&gop_guid, 0, (void **)&gop)) || !gop) {
        print("[boot] no graphics output: serial console only\n");
        return;
    }

    /* Preference: video=WxH from the command line, then 1280x800, then the largest mode up to 1920x1080. */
    UINT32 want_w = 1280, want_h = 800;
    parse_video(bi->cmdline, &want_w, &want_h);
    UINT32 best = gop->Mode->Mode, best_area = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN size;
        if (EFI_ERROR(gop->QueryMode(gop, m, &size, &info))) continue;
        if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
            info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
            continue;
        UINT32 w = info->HorizontalResolution, h = info->VerticalResolution;
        if (w == want_w && h == want_h) {
            best = m;
            break;
        }
        if (w <= 1920 && h <= 1080 && w * h > best_area) {
            best = m;
            best_area = w * h;
        }
    }
    if (best != gop->Mode->Mode && EFI_ERROR(gop->SetMode(gop, best)))
        print("[boot] could not change the video mode, keeping the current one\n");

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    struct boot_framebuffer *fb = &bi->fb;
    fb->base_phys = gop->Mode->FrameBufferBase;
    fb->base = BOOT_HHDM_BASE + fb->base_phys;
    fb->size = gop->Mode->FrameBufferSize;
    fb->width = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch = info->PixelsPerScanLine * 4;
    fb->bpp = 32;
    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        fb->red_shift = 0; fb->green_shift = 8; fb->blue_shift = 16;
    } else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        fb->red_shift = 16; fb->green_shift = 8; fb->blue_shift = 0;
    } else {
        fb->red_shift = (UINT8)shift_of(info->PixelInformation.RedMask);
        fb->green_shift = (UINT8)shift_of(info->PixelInformation.GreenMask);
        fb->blue_shift = (UINT8)shift_of(info->PixelInformation.BlueMask);
    }
}

/* ---- page tables ----------------------------------------------------- */

#define PTE_PRESENT 0x001ULL
#define PTE_WRITE   0x002ULL
#define PTE_HUGE    0x080ULL
#define PTE_ADDR    0x000FFFFFFFFFF000ULL

static UINT64 pt_pool, pt_pool_end;

static UINT64 *pt_alloc(void) {
    if (pt_pool >= pt_pool_end) fail("page table pool exhausted", 0);
    UINT64 *t = (UINT64 *)pt_pool;
    pt_pool += PAGE_SIZE;
    return t;   /* already zeroed */
}

/* Returns the next-level table for index `i` of `table`, creating it if needed. */
static UINT64 *pt_next(UINT64 *table, UINTN i) {
    if (!(table[i] & PTE_PRESENT)) table[i] = (UINT64)pt_alloc() | PTE_PRESENT | PTE_WRITE;
    return (UINT64 *)(table[i] & PTE_ADDR);
}

static void map_2m(UINT64 *pml4, UINT64 virt, UINT64 phys) {
    UINT64 *pdpt = pt_next(pml4, (virt >> 39) & 511);
    UINT64 *pd = pt_next(pdpt, (virt >> 30) & 511);
    pd[(virt >> 21) & 511] = phys | PTE_PRESENT | PTE_WRITE | PTE_HUGE;
}

static void map_4k(UINT64 *pml4, UINT64 virt, UINT64 phys) {
    UINT64 *pdpt = pt_next(pml4, (virt >> 39) & 511);
    UINT64 *pd = pt_next(pdpt, (virt >> 30) & 511);
    UINT64 *pt = pt_next(pd, (virt >> 21) & 511);
    pt[(virt >> 12) & 511] = phys | PTE_PRESENT | PTE_WRITE;
}

/* ---- memory map ------------------------------------------------------ */

static UINT8 *mmap_buf;
static UINTN mmap_buf_size, mmap_size, mmap_key, desc_size;
static UINT32 desc_version;

static EFI_STATUS get_memory_map(void) {
    mmap_size = mmap_buf_size;
    return BS->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf, &mmap_key, &desc_size, &desc_version);
}

static UINT32 region_type(UINT32 efi_type) {
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:      return BOOT_MEM_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:            return BOOT_MEM_BOOTLOADER;
    case EfiACPIReclaimMemory:     return BOOT_MEM_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:         return BOOT_MEM_ACPI_NVS;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace: return BOOT_MEM_MMIO;
    case EfiUnusableMemory:        return BOOT_MEM_BAD;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:   return BOOT_MEM_FIRMWARE_RUNTIME;
    default:                       return BOOT_MEM_RESERVED;
    }
}

/* Inserts a region at index i (shifting the rest). */
static void region_insert(struct boot_mem_region *r, UINT64 *n, UINT64 i, UINT64 base, UINT64 len, UINT32 type) {
    if (*n >= MAX_REGIONS) return;
    for (UINT64 j = *n; j > i; j--) r[j] = r[j - 1];
    r[i].base = base;
    r[i].length = len;
    r[i].type = type;
    r[i].reserved = 0;
    (*n)++;
}

/* Gives [base, base+len) its own type, splitting the region that contains it. */
static void carve(struct boot_mem_region *r, UINT64 *n, UINT64 base, UINT64 len, UINT32 type) {
    for (UINT64 i = 0; i < *n; i++) {
        UINT64 rb = r[i].base, re = r[i].base + r[i].length;
        if (base < rb || base + len > re) continue;
        UINT32 old = r[i].type;
        r[i].length = base - rb;                                     /* part before (may become empty) */
        region_insert(r, n, i + 1, base, len, type);                 /* the carved part */
        if (base + len < re) region_insert(r, n, i + 2, base + len, re - base - len, old);   /* part after */
        if (r[i].length == 0) {                                      /* drop the empty part before */
            for (UINT64 j = i; j + 1 < *n; j++) r[j] = r[j + 1];
            (*n)--;
        }
        return;
    }
}

/* Converts the UEFI map into sorted boot_mem_region[]: kernel and boot data carved out,
   neighbours of the same type merged. Runs after ExitBootServices: no allocation. */
static UINT64 convert_memory_map(struct boot_mem_region *out) {
    UINT64 n = 0;
    for (UINTN off = 0; off < mmap_size && n < MAX_REGIONS; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap_buf + off);
        region_insert(out, &n, n, d->PhysicalStart, d->NumberOfPages * PAGE_SIZE, region_type(d->Type));
    }
    /* the UEFI map is not guaranteed to be sorted: insertion sort by base */
    for (UINT64 i = 1; i < n; i++) {
        struct boot_mem_region r = out[i];
        UINT64 j = i;
        while (j > 0 && out[j - 1].base > r.base) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = r;
    }
    for (int i = 0; i < owned_count; i++) carve(out, &n, owned[i].base, owned[i].length, owned[i].type);
    UINT64 m = 0;
    for (UINT64 i = 0; i < n; i++) {
        if (m && out[m - 1].type == out[i].type && out[m - 1].base + out[m - 1].length == out[i].base)
            out[m - 1].length += out[i].length;
        else
            out[m++] = out[i];
    }
    return m;
}

/* ---- entry point ----------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    ST = system_table;
    BS = system_table->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    serial_init();
    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->SetAttribute(ST->ConOut, EFI_LIGHTMAGENTA | EFI_BACKGROUND_BLACK);
    print("[boot] OpalineOS bootloader " BOOTLOADER_VERSION "\n");
    ST->ConOut->SetAttribute(ST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);

    /* boot_info and the region array live in one block of boot data pages */
    UINT64 bi_pages = (sizeof(struct boot_info) + MAX_REGIONS * sizeof(struct boot_mem_region) + PAGE_SIZE - 1) / PAGE_SIZE;
    struct boot_info *bi = (struct boot_info *)alloc_pages(bi_pages, BOOT_MEM_BOOT_DATA, "no memory for boot_info");
    struct boot_mem_region *regions = (struct boot_mem_region *)(bi + 1);
    bi->magic = BOOT_INFO_MAGIC;
    bi->version = BOOT_INFO_VERSION;
    bi->hhdm_base = BOOT_HHDM_BASE;
    bi->firmware_revision = ST->FirmwareRevision;
    for (int i = 0; i < 63 && ST->FirmwareVendor && ST->FirmwareVendor[i]; i++) {
        CHAR16 c = ST->FirmwareVendor[i];
        bi->firmware_vendor[i] = c < 128 ? (char)c : '?';
    }

    /* the partition this program was loaded from */
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS s = BS->HandleProtocol(image, &li_guid, (void **)&li);
    if (EFI_ERROR(s)) fail("no loaded image protocol", s);
    s = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(s)) fail("boot partition has no file system", s);
    s = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(s)) fail("could not open the boot partition", s);

    UINT64 size = 0;
    UINT64 cmd = read_file(u"\\OPALINE\\CMDLINE.TXT", 0, &size);
    if (cmd) {
        UINT64 n = 0;
        for (UINT64 i = 0; i < size && n < sizeof bi->cmdline - 1; i++) {
            char c = ((char *)cmd)[i];
            if (c == '\r' || c == '\n') c = ' ';
            bi->cmdline[n++] = c;
        }
        while (n && bi->cmdline[n - 1] == ' ') n--;
        bi->cmdline[n] = 0;
        print("[boot] command line: ");
        print(bi->cmdline);
        print("\n");
    }

    UINT64 font = read_file(u"\\OPALINE\\CONSOLE.FNT", BOOT_MEM_BOOT_DATA, &size);
    if (font) {
        bi->font = BOOT_HHDM_BASE + font;
        bi->font_size = size;
    } else {
        print("[boot] \\OPALINE\\CONSOLE.FNT not found: no text on screen\n");
    }

    print("[boot] loading \\OPALINE\\KERNEL.ELF\n");
    UINT64 elf = read_file(u"\\OPALINE\\KERNEL.ELF", 0, &size);
    if (!elf) fail("\\OPALINE\\KERNEL.ELF not found", EFI_NOT_FOUND);
    load_kernel(elf, size);
    bi->kernel_phys = kernel_phys;
    bi->kernel_size = kernel_pages * PAGE_SIZE;
    print("[boot] kernel: ");
    print_dec(kernel_pages * 4);
    print(" KiB at ");
    print_hex(kernel_phys);
    print(", entry ");
    print_hex(kernel_entry);
    print("\n");

    /* ACPI root pointer (for later stages) */
    EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID, acpi10 = EFI_ACPI_10_TABLE_GUID;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_equal(&t->VendorGuid, &acpi20)) bi->rsdp_phys = (UINT64)t->VendorTable;
        else if (!bi->rsdp_phys && guid_equal(&t->VendorGuid, &acpi10)) bi->rsdp_phys = (UINT64)t->VendorTable;
    }

    setup_graphics(bi);

    /* buffer for the final memory map (with room for the allocations made below) */
    mmap_buf_size = 0;
    BS->GetMemoryMap(&mmap_buf_size, 0, &mmap_key, &desc_size, &desc_version);
    mmap_buf_size += 64 * desc_size;
    s = BS->AllocatePool(EfiLoaderData, mmap_buf_size, (void **)&mmap_buf);
    if (EFI_ERROR(s)) fail("no memory for the memory map", s);
    s = get_memory_map();
    if (EFI_ERROR(s)) fail("GetMemoryMap failed", s);

    /*
     * Highest physical address to map: all of the first 4 GiB (RAM and the usual MMIO),
     * RAM above it and the framebuffer, rounded up to 1 GiB. Reserved/MMIO ranges above
     * 4 GiB (QEMU reports one near 1 TiB) are mapped by the kernel when a driver needs them.
     */
    UINT64 max_phys = 4ULL << 30;
    for (UINTN off = 0; off < mmap_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap_buf + off);
        if (d->Type == EfiReservedMemoryType || d->Type == EfiMemoryMappedIO ||
            d->Type == EfiMemoryMappedIOPortSpace)
            continue;
        UINT64 end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (end > max_phys) max_phys = end;
    }
    if (bi->fb.base_phys + bi->fb.size > max_phys) max_phys = bi->fb.base_phys + bi->fb.size;
    max_phys = (max_phys + (1ULL << 30) - 1) & ~((1ULL << 30) - 1);
    if (max_phys > (512ULL << 30)) max_phys = 512ULL << 30;   /* one PDPT per region is enough for now */

    UINT64 gib = max_phys >> 30;
    UINT64 pt_pages = 1 /* PML4 */ + 2 * (1 + gib) /* identity + HHDM: PDPT + PDs */ +
                      2 /* kernel PDPT + PD */ + (kernel_pages + 511) / 512 + 1 /* kernel PTs */;
    pt_pool = alloc_pages(pt_pages, BOOT_MEM_BOOT_DATA, "no memory for page tables");
    pt_pool_end = pt_pool + pt_pages * PAGE_SIZE;
    UINT64 *pml4 = pt_alloc();
    for (UINT64 p = 0; p < max_phys; p += 2ULL << 20) {
        map_2m(pml4, p, p);
        map_2m(pml4, BOOT_HHDM_BASE + p, p);
    }
    for (UINT64 i = 0; i < kernel_pages; i++)
        map_4k(pml4, kernel_vbase + i * PAGE_SIZE, kernel_phys + i * PAGE_SIZE);

    bi->mem_regions = BOOT_HHDM_BASE + (UINT64)regions;
    print("[boot] mapped ");
    print_dec(gib);
    print(" GiB, starting the kernel\n");

    /* ExitBootServices fails if the map changed since GetMemoryMap: then fetch it again and retry once. */
    s = get_memory_map();
    if (!EFI_ERROR(s)) s = BS->ExitBootServices(image, mmap_key);
    if (EFI_ERROR(s)) {
        s = get_memory_map();
        if (!EFI_ERROR(s)) s = BS->ExitBootServices(image, mmap_key);
        if (EFI_ERROR(s)) fail("ExitBootServices failed", s);
    }
    console_alive = 0;

    bi->mem_region_count = convert_memory_map(regions);

    UINT64 bi_virt = BOOT_HHDM_BASE + (UINT64)bi;
    __asm__ volatile(
        "cli\n\t"
        "mov %0, %%cr3\n\t"
        "mov %1, %%rdi\n\t"
        "xor %%ebp, %%ebp\n\t"
        "jmp *%2\n\t"
        :
        : "r"((UINT64)pml4), "r"(bi_virt), "r"(kernel_entry)
        : "rdi", "memory");
    __builtin_unreachable();
}
