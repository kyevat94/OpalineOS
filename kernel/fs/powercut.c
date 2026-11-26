/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Power-cut test (stage SE2), started by the command line "test=powercut:N".
 *
 * Runs a fixed file system workload in /home/pc; after N block writes the disk
 * "loses power" (block_powercut_arm: later writes are silently dropped) and the
 * machine stops without flushing or unmounting anything. The test harness then
 * boots the same disk normally: the EFS3 journal must bring it back to a
 * consistent state, which the host's fsck (tools/efs3.py check) verifies.
 * N = 0 runs the whole workload and reports how many writes it makes.
 */
#include <opaline/block.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/serial.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

static void write_file(const char *path, u64 size, u8 seed, bool keep_open, struct file **kept) {
    struct file *f;
    if (vfs_open(path, NULL, O_CREAT | O_RDWR | O_TRUNC, 0644, &f) < 0) return;
    u8 *buf = kmalloc(64 * KiB);
    for (u64 done = 0; buf && done < size;) {
        u64 n = MIN(size - done, 64 * KiB);
        for (u64 i = 0; i < n; i++) buf[i] = (u8)(seed + (done + i) * 7);
        if (file_write(f, buf, n) <= 0) break;
        done += n;
    }
    kfree(buf);
    if (keep_open) *kept = f;
    else file_put(f);
}

void powercut_test(u64 cut_after) {
    block_powercut_arm(cut_after ? cut_after : ~0ULL);
    kprintf("POWERCUT test: the workload starts (the power goes off after %lu writes)\n", cut_after);
    char path[64];
    vfs_mkdir("/home/pc", NULL, 0755);
    write_file("/home/pc/a.txt", 100, 1, false, NULL);
    write_file("/home/pc/big.bin", 300 * KiB, 2, false, NULL);   /* indirect blocks */
    vfs_symlink("a.txt", "/home/pc/link", NULL);
    vfs_link("/home/pc/a.txt", NULL, "/home/pc/hard.txt", NULL, false);
    vfs_rename("/home/pc/a.txt", "/home/pc/b.txt", NULL);
    vfs_mkdir("/home/pc/sub", NULL, 0755);
    write_file("/home/pc/sub/c.txt", 9000, 3, false, NULL);
    vfs_mkdir("/home/pc/d1", NULL, 0755);
    vfs_mkdir("/home/pc/d1/inner", NULL, 0755);
    write_file("/home/pc/d1/inner/x.txt", 2000, 4, false, NULL);
    vfs_mkdir("/home/pc/d2", NULL, 0755);
    vfs_rename("/home/pc/d1/inner", "/home/pc/d2/inner", NULL);   /* another parent: "..", two link counts */
    vfs_mkdir("/home/pc/empty", NULL, 0755);
    vfs_rename("/home/pc/d2/inner", "/home/pc/empty", NULL);      /* over an empty directory */
    struct file *f;
    if (vfs_open("/home/pc/big.bin", NULL, O_WRONLY, 0, &f) == 0) {
        f->vn->ops->truncate(f->vn, 10000);   /* frees most of its blocks */
        file_put(f);
    }
    vfs_remove("/home/pc/hard.txt", NULL, false);
    for (int i = 0; i < 20; i++) {
        ksnprintf(path, sizeof path, "/home/pc/f%02d", i);
        write_file(path, 1000 + (u64)i * 300, (u8)i, false, NULL);
    }
    for (int i = 0; i < 20; i += 2) {
        ksnprintf(path, sizeof path, "/home/pc/f%02d", i);
        vfs_remove(path, NULL, false);
    }
    write_file("/home/pc/new.txt", 500, 9, false, NULL);
    vfs_rename("/home/pc/new.txt", "/home/pc/b.txt", NULL);   /* replaces b.txt */
    vfs_remove("/home/pc/sub/c.txt", NULL, false);
    vfs_remove("/home/pc/sub", NULL, true);
    /* deleted while open: only the next mount can free it */
    struct file *orphan = NULL;
    write_file("/home/pc/orphan.bin", 40 * KiB, 5, true, &orphan);
    vfs_remove("/home/pc/orphan.bin", NULL, false);
    if (orphan) file_write(orphan, "still open", 10);
    kprintf("POWERCUT workload finished: %lu writes%s\n", block_writes(),
            block_powercut_hit() ? " (the disk lost the ones after the cut)" : "");
    static const char marker[] = "TEST_OK powercut\n";
    serial_write(marker, sizeof marker - 1);
    for (;;) __asm__ volatile("cli\n\thlt");   /* the power is off: nothing else may reach the disk */
}
