/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* User programs embedded in the kernel image until there is a file system (stage E5).
   The table comes from build/gen/user_programs.asm, written by tools/build.py. */
#include <opaline/proc.h>
#include <opaline/string.h>

extern const struct embedded_program embedded_programs[];
extern const u64 embedded_program_count;

const struct embedded_program *embedded_program_find(const char *name) {
    for (u64 i = 0; i < embedded_program_count; i++)
        if (!strcmp(embedded_programs[i].name, name)) return &embedded_programs[i];
    return NULL;
}
