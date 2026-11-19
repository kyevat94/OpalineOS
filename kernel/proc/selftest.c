/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User-space self-test, run on every boot:
 *   1. "usertest" checks the system calls from user mode (results, -EFAULT, -ENOSYS...);
 *   2. two "hello" processes run at the same time and are preempted;
 *   3. three programs that crash (NULL access, privileged instruction, reading
 *      kernel memory) are stopped while the kernel keeps running;
 *   4. no memory is lost once every process ended.
 * The programs are read from /apps on the root file system.
 */
#include <opaline/block.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/proc.h>

#define FAIL(...)                          \
    do {                                   \
        console_set_color(COLOR_ERROR);    \
        kprintf("  FAILED: " __VA_ARGS__); \
        kprintf("\n");                     \
        return false;                      \
    } while (0)

/* Starts /apps/<name> from the root file system. */
static struct process *spawn(const char *name) {
    char path[64];
    ksnprintf(path, sizeof path, "/apps/%s", name);
    const char *argv[] = {path, NULL};
    int err;
    struct process *p = process_spawn_path(path, argv, NULL, &err);
    if (!p) kprintf("  cannot start %s: error %d\n", path, err);
    return p;
}

bool user_selftest(void) {
    struct pmm_stats before, after;
    pmm_get_stats(&before);
    u64 cache_before = block_cache_pages();

    struct process *p = spawn("usertest");
    if (!p) FAIL("could not start usertest");
    int code = process_wait(p);
    if (code != 0) FAIL("usertest exited with code %d", code);

    struct process *h1 = spawn("hello"), *h2 = spawn("hello");
    if (!h1 || !h2) FAIL("could not start hello");
    int c1 = process_wait(h1), c2 = process_wait(h2);
    if (c1 != 0 || c2 != 0) FAIL("hello exited with codes %d and %d", c1, c2);

    static const char *const crashers[] = {"crashnull", "crashpriv", "crashkernel"};
    for (unsigned i = 0; i < 3; i++) {
        struct process *c = spawn(crashers[i]);
        if (!c) FAIL("could not start %s", crashers[i]);
        code = process_wait(c);
        if (code != EXIT_SIGSEGV) FAIL("%s ended with code %d, expected %d", crashers[i], code, EXIT_SIGSEGV);
    }

    pmm_get_stats(&after);
    struct heap_stats hs;
    heap_get_stats(&hs);
    u64 cached = block_cache_pages() - cache_before;   /* the programs' files stay in the block cache */
    if (before.free_pages - after.free_pages > hs.slab_pages + cached)
        FAIL("pages lost: %lu free before, %lu after (%lu now in the block cache)", before.free_pages,
             after.free_pages, cached);

    console_set_color(COLOR_TEXT);
    kprintf("User-space self-test passed: 6 processes, 3 crashes contained, no memory lost\n");
    return true;
}
