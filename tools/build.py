# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""Builds OpalineOS.

    python tools/build.py           # compile and create build/opaline.img
    python tools/build.py --clean   # delete build/ first

Outputs (in build/):
    BOOTX64.EFI     the UEFI bootloader
    user/*.elf      user programs (embedded in the kernel until there is a file system)
    KERNEL.ELF      the kernel
    opaline.img     raw GPT disk image (128 MiB) with the EFI System Partition
    opaline.vmdk    VirtualBox descriptor pointing at opaline.img (created once)

Other tools import make_image() to build image variants (e.g. with a test command line).
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
TOOLS = os.path.join(ROOT, "tools")
IMAGE = os.path.join(BUILD, "opaline.img")
VMDK = os.path.join(BUILD, "opaline.vmdk")
BOOTLOADER = os.path.join(BUILD, "BOOTX64.EFI")
KERNEL = os.path.join(BUILD, "KERNEL.ELF")
FONT = os.path.join(ROOT, "assets", "fonts", "console.fnt")
USER_BUILD = os.path.join(BUILD, "user")
GEN = os.path.join(BUILD, "gen")

LLVM_DIRS = [r"C:\Program Files\LLVM\bin"]
NASM_DIRS = [r"C:\Program Files\NASM", os.path.join(os.environ.get("LOCALAPPDATA", ""), "bin", "NASM")]

WARNINGS = ["-Wall", "-Wextra", "-Werror"]

USER_CFLAGS = [
    "--target=x86_64-unknown-elf", "-std=c11", "-O2", "-g0",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-fno-omit-frame-pointer",
    "-I", os.path.join(ROOT, "user", "include"),
] + WARNINGS

EFI_CFLAGS = [
    "--target=x86_64-unknown-windows", "-std=c11", "-O2",
    "-ffreestanding", "-fno-stack-protector", "-fshort-wchar", "-fno-builtin",
    "-mno-red-zone", "-mno-stack-arg-probe", "-mgeneral-regs-only",
] + WARNINGS

KERNEL_CFLAGS = [
    "--target=x86_64-unknown-elf", "-std=c11", "-O2", "-g0",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-mcmodel=kernel", "-mno-red-zone", "-mgeneral-regs-only",
    "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer",
    "-I", os.path.join(ROOT, "kernel", "include"), "-I", ROOT,
] + WARNINGS


def tool(name, dirs):
    """Finds a tool on PATH or in the usual install folders."""
    found = shutil.which(name)
    if found:
        return found
    for d in dirs:
        cand = os.path.join(d, name + ".exe")
        if os.path.isfile(cand):
            return cand
    sys.exit(f"ERROR: '{name}' not found. Install it (see README.md).")


def run(cmd, quiet=False):
    if not quiet:
        shown = [os.path.basename(cmd[0])] + [os.path.relpath(c, ROOT) if os.path.isabs(c) else c for c in cmd[1:]]
        print("  " + " ".join(shown), flush=True)
    result = subprocess.run(cmd)
    if result.returncode != 0:
        sys.exit(f"ERROR: command failed ({result.returncode})")


def sources(folder, ext):
    out = []
    for dirpath, _, files in os.walk(folder):
        out += [os.path.join(dirpath, f) for f in files if f.endswith(ext)]
    return sorted(out)


def obj_path(src, kind):
    rel = os.path.relpath(src, ROOT).replace(os.sep, "_").replace(".", "_")
    return os.path.join(BUILD, "obj", kind, rel + ".o")


def build_bootloader():
    clang = tool("clang", LLVM_DIRS)
    lld_link = tool("lld-link", LLVM_DIRS)
    objs = []
    for src in sources(os.path.join(ROOT, "boot", "efi"), ".c"):
        obj = obj_path(src, "boot")
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        run([clang] + EFI_CFLAGS + ["-c", src, "-o", obj])
        objs.append(obj)
    run([lld_link, "/nologo", "/subsystem:efi_application", "/entry:efi_main",
         "/nodefaultlib", "/out:" + BOOTLOADER] + objs)


def build_user():
    """Builds every program in user/<name>/ into build/user/<name>.elf (static, linked with user/lib)."""
    clang = tool("clang", LLVM_DIRS)
    ld = tool("ld.lld", LLVM_DIRS)
    nasm = tool("nasm", NASM_DIRS)
    udir = os.path.join(ROOT, "user")
    os.makedirs(USER_BUILD, exist_ok=True)

    def compile_dir(folder):
        objs = []
        for src in sources(folder, ".asm"):
            obj = obj_path(src, "user")
            os.makedirs(os.path.dirname(obj), exist_ok=True)
            run([nasm, "-f", "elf64", "-o", obj, src])
            objs.append(obj)
        for src in sources(folder, ".c"):
            obj = obj_path(src, "user")
            os.makedirs(os.path.dirname(obj), exist_ok=True)
            run([clang] + USER_CFLAGS + ["-c", src, "-o", obj])
            objs.append(obj)
        return objs

    lib = compile_dir(os.path.join(udir, "lib"))
    programs = []
    for name in sorted(os.listdir(udir)):
        folder = os.path.join(udir, name)
        if not os.path.isdir(folder) or name in ("lib", "include"):
            continue
        out = os.path.join(USER_BUILD, name + ".elf")
        run([ld, "-nostdlib", "-static", "-z", "max-page-size=4096", "-z", "noexecstack",
             "-T", os.path.join(udir, "linker.ld"), "-o", out] + compile_dir(folder) + lib)
        programs.append(name)
    return programs


def write_program_table(programs):
    """Generates build/gen/user_programs.asm: the ELF files plus a table the kernel searches by name
    (struct embedded_program in kernel/include/opaline/proc.h)."""
    os.makedirs(GEN, exist_ok=True)
    lines = ["; generated by tools/build.py", "bits 64", "section .rodata", "align 8"]
    for i, name in enumerate(programs):
        lines += [f"name_{i}: db \"{name}\", 0", "align 16",
                  f"blob_{i}: incbin \"{name}.elf\"", f"blob_{i}_end:", "align 8"]
    lines += ["global embedded_programs, embedded_program_count", "embedded_programs:"]
    lines += [f"    dq name_{i}, blob_{i}, blob_{i}_end" for i in range(len(programs))]
    lines += [f"embedded_program_count: dq {len(programs)}", ""]
    path = os.path.join(GEN, "user_programs.asm")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    return path


def build_kernel(programs):
    clang = tool("clang", LLVM_DIRS)
    ld = tool("ld.lld", LLVM_DIRS)
    nasm = tool("nasm", NASM_DIRS)
    kdir = os.path.join(ROOT, "kernel")
    objs = []
    for src in sources(kdir, ".asm") + [write_program_table(programs)]:
        obj = obj_path(src, "kernel")
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        run([nasm, "-f", "elf64", "-I", USER_BUILD + os.sep, "-o", obj, src])
        objs.append(obj)
    for src in sources(kdir, ".c"):
        obj = obj_path(src, "kernel")
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        run([clang] + KERNEL_CFLAGS + ["-c", src, "-o", obj])
        objs.append(obj)
    run([ld, "-nostdlib", "-static", "-z", "max-page-size=4096", "-z", "noexecstack",
         "-T", os.path.join(kdir, "linker.ld"), "-o", KERNEL] + objs)


def make_image(path, cmdline=None):
    """Writes a disk image with the bootloader, kernel and font (and CMDLINE.TXT if given)."""
    files = ["EFI/BOOT/BOOTX64.EFI=" + BOOTLOADER, "OPALINE/KERNEL.ELF=" + KERNEL,
             "OPALINE/CONSOLE.FNT=" + FONT]
    if cmdline is not None:
        cmd_file = os.path.splitext(path)[0] + ".cmdline.txt"
        with open(cmd_file, "w", newline="\n") as f:
            f.write(cmdline + "\n")
        files.append("OPALINE/CMDLINE.TXT=" + cmd_file)
    args = [sys.executable, os.path.join(TOOLS, "mkimage.py"), path]
    for spec in files:
        args += ["--file", spec]
    run(args, quiet=True)


def write_vmdk():
    """A 'monolithicFlat' VMDK lets VirtualBox use the raw image directly.
    Written only once: VirtualBox stores its own UUID in this file."""
    if os.path.isfile(VMDK):
        return
    sectors = os.path.getsize(IMAGE) // 512
    cylinders = min(sectors // (16 * 63), 16383)
    with open(VMDK, "w", newline="\n") as f:
        f.write(f"""# Disk DescriptorFile
version=1
CID=0a11e05e
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW {sectors} FLAT "{os.path.basename(IMAGE)}" 0

# The Disk Data Base
#DDB

ddb.virtualHWVersion = "4"
ddb.adapterType="ide"
ddb.geometry.cylinders="{cylinders}"
ddb.geometry.heads="16"
ddb.geometry.sectors="63"
""")


def main():
    ap = argparse.ArgumentParser(description="Build OpalineOS")
    ap.add_argument("--clean", action="store_true", help="delete build/ first (the VirtualBox descriptor is kept)")
    args = ap.parse_args()

    if args.clean and os.path.isdir(BUILD):
        for name in os.listdir(BUILD):
            if name == os.path.basename(VMDK):
                continue
            path = os.path.join(BUILD, name)
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    os.makedirs(BUILD, exist_ok=True)

    print("[1/4] bootloader", flush=True)
    build_bootloader()
    print("[2/4] user programs", flush=True)
    programs = build_user()
    print("[3/4] kernel", flush=True)
    build_kernel(programs)
    print("[4/4] disk image", flush=True)
    make_image(IMAGE)
    write_vmdk()
    print(f"OK: {os.path.relpath(IMAGE, ROOT)}")


if __name__ == "__main__":
    main()
