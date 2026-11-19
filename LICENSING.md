# Licensing

OpalineOS is free software. Copyright (C) 2026 The OpalineOS authors.

| Part of the tree | License | Text |
|---|---|---|
| Everything, unless listed below: kernel, bootloader, compositor, shell, system apps, build tools | **GPL-3.0-or-later** | [`COPYING`](COPYING) |
| Libraries that apps import: `system/opaline/` (the `opaline` Python package, including `opaline.vitrum`, `opaline.system` and `opaline.i18n`) and `system/vitrum/` (the `_vitrum` C core) | **LGPL-3.0-or-later** | [`COPYING.LESSER`](COPYING.LESSER) together with [`COPYING`](COPYING) |
| User-space library for the first programs: `user/include/` and `user/lib/` | **LGPL-3.0-or-later** | [`COPYING.LESSER`](COPYING.LESSER) together with [`COPYING`](COPYING) |
| `assets/fonts/console.fnt` (console font, a bitmap rendering of DejaVu Sans Mono) | Bitstream Vera Fonts license + public domain (DejaVu changes) | [`assets/fonts/LICENSE-DejaVu.txt`](assets/fonts/LICENSE-DejaVu.txt) |
| Third-party code in `ports/` | its own upstream license | in each port |

Each source file states its license on the first lines with an SPDX identifier, for example
`SPDX-License-Identifier: GPL-3.0-or-later`. The SPDX line is authoritative for that file.

## What this means

- **GPL-3.0-or-later**: you may use, study, change and share OpalineOS. If you distribute a modified
  version, you must make its source code available under the same license. "Or later" means any later
  version of the GPL published by the Free Software Foundation may also be used.
- **LGPL-3.0-or-later** (libraries): apps that only *use* these libraries (for example by
  `from opaline import vitrum`) may be released under **any license**, including proprietary ones.
  Changes to the libraries themselves must be shared under the LGPL.

## Programs that use the kernel through system calls

The kernel is GPL-3.0-or-later. Programs that use the kernel's services only through the normal
system call interface are **not** considered derivative works of the kernel, and their license is
not affected by the kernel's license. (This is the same clarification the Linux kernel makes.)
