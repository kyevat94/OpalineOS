# OpalineOS

A light, fast and modern operating system with its own kernel, where **Python is at the center**:
the desktop and the apps are written in Python (official CPython) using `opaline-relief`, the
system's GUI toolkit.

> **Status:** stage **E6** — 64-bit kernel with memory management, a preemptive scheduler,
> isolated user processes, storage (AHCI disk, the EFS3 file system with UTF-8 file names), pipes,
> a terminal and a small shell, plus a **POSIX layer**: ordinary C programs built against
> **musl** run unmodified, statically linked or through the dynamic linker, with `fork`/`execve`,
> `mmap`, signals and threads. Since sub-stage **SE1**: copy-on-write `fork`, shared memory, the real
> clock, users and POSIX permissions, and job control (Ctrl+C / Ctrl+Z in the shell). Since **SE2**:
> a journaled file system that survives power cuts, and symbolic and hard links. Since **SE3**:
> terminal settings (canonical and raw mode), `select`, non-blocking I/O, and a new shell written
> against musl that runs programs with `fork`/`execve` and passes them its environment variables.
> Self-tests for memory, scheduling, user space, files and POSIX run on every boot.

![Shell](docs/screenshots/e5-shell.png)
> The full plan is in [`project.md`](project.md) (in Portuguese) and the toolkit API in
> [`opaline-relief.md`](opaline-relief.md).

## Requirements (Windows)

- Python 3
- LLVM (`clang`, `lld-link`): `winget install LLVM.LLVM`
- NASM (from the next stages on): `winget install NASM.NASM`
- QEMU (includes the OVMF UEFI firmware): `winget install SoftwareFreedomConservancy.QEMU`
- VirtualBox (optional, second test machine)

## Build, run and test

```
python tools/build.py              # builds build/opaline.img (add --clean for a fresh build;
                                   # the first build downloads musl 1.2.6 and checks its SHA-256)
python tools/vm.py run             # boots in QEMU (window)
python tools/vm.py run vbox        # boots in VirtualBox (creates the "OpalineOS" VM the first time)
python tools/vm.py test            # automated boot and exception tests in QEMU and VirtualBox
python tools/vm.py run --cmdline test=pagefault   # see the panic screen
python tools/efs3.py tree build/opaline.img        # list the files on the disk image (from the host)
```

The test prints `TEST_OK` / `TEST_FAILED` per emulator and exits with a non-zero code on failure.

## License

**GPL-3.0-or-later** for the system and **LGPL-3.0-or-later** for the libraries that apps import
(`opaline.relief`, `opaline.system`, `opaline.i18n`), so apps for OpalineOS can use any license.
See [`LICENSING.md`](LICENSING.md), [`COPYING`](COPYING) and [`COPYING.LESSER`](COPYING.LESSER).
